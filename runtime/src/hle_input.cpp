// hle_input.cpp - the event broker: the OS service that delivers controller
// events to games. On a real 3DO it is a separate task with the message port
// "eventbroker". Here it is answered directly:
//
//   game  --SendMsg(EB_Configure)-->  broker     register as listener, reply
//   broker --SendMsg(EB_EventRecord)--> game     once per frame when the pad changes
//   game  --ReplyMsg-->               broker     message goes back to the pool
#include "hle_os.h"
#include "tdort/platform.h"
#include <algorithm>
#include <vector>

namespace tdort {

using namespace abi;

static uint32_t to_3do_pad(uint32_t host) {
    static const std::pair<uint32_t, uint32_t> map[] = {
        {PadUp, kCtlUp}, {PadDown, kCtlDown}, {PadLeft, kCtlLeft}, {PadRight, kCtlRight},
        {PadA, kCtlA}, {PadB, kCtlB}, {PadC, kCtlC}, {PadP, kCtlStart}, {PadX, kCtlX},
        {PadL, kCtlLeftShift}, {PadR, kCtlRightShift},
    };
    uint32_t out = 0;
    for (auto& [h, t] : map) if (host & h) out |= t;
    return out;
}

void Portfolio::install_input() {
    HleItem& p = new_item(ItemKind::MsgPort, kSubsysKernel, kNodeMsgPort, msgport::Size, "eventbroker", false);
    list_init(p.addr + msgport::Msgs, 0);
    broker_port_ = p.num;
}

uint32_t Portfolio::listener_signals() const {
    uint32_t s = 0;
    for (auto& l : listeners_)
        if (l.port > 0 && size_t(l.port) < items_.size()) s |= items_[size_t(l.port)].signal;
    return s;
}

bool Portfolio::blocking_port(int32_t port) const {
    for (auto& l : listeners_) if (l.port == port) return true;
    return false;
}

// A message the game sent to the broker. Returns true when handled.
bool Portfolio::broker_receive(HleItem& msg, uint32_t data, uint32_t size) {
    const uint32_t flavor = size >= 4 ? r32(data) : 0;
    int32_t reply_to = msg.reply_port ? msg.reply_port : int32_t(r32(msg.addr + message::ReplyPort));
    int32_t result = 0;
    if (flavor == kEbConfigure) {
        Listener l;
        l.port = reply_to;
        l.trigger = size >= evconfig::TriggerMask + 4 ? r32(data + evconfig::TriggerMask) : 0;
        l.capture = size >= evconfig::CaptureMask + 4 ? r32(data + evconfig::CaptureMask) : 0;
        bool replaced = false;
        for (auto& e : listeners_) if (e.port == l.port) { e = l; replaced = true; }
        if (!replaced) listeners_.push_back(l);
        rt.log("event broker: listener port %d configured (trigger %08X, capture %08X)", l.port, l.trigger, l.capture);
    } else if (flavor == 13) {                 // EB_SetFocus { flavor, listener }: reply = the focused listener
        if (size >= 8) broker_focus_ = int32_t(r32(data + 4));
        result = broker_focus_;
    } else if (flavor == 15) {                 // EB_GetFocus
        result = broker_focus_ ? broker_focus_ : (listeners_.empty() ? 0 : listeners_.front().port);
    } else if (flavor == 27) {                 // EB_DescribePods: one standard control pad
        // Reply data goes in the message's own buffer: { EB_DescribePodsReply, count, PodDescription[] }.
        // (a pass-by-value message: its own area; else the buffer the sender gave)
        const uint32_t buf = msg.msg_buffer ? msg.msg_buffer : r32(msg.addr + message::DataPtr);
        const uint32_t room = msg.msg_buffer ? msg.msg_buffer_size : r32(msg.addr + message::DataPtrSize);
        constexpr uint32_t kPodSize = 40, kLen = 8 + kPodSize;
        if (buf && room >= kLen && guest_range_ok(buf, kLen)) {
            std::vector<uint8_t> z(kLen, 0);
            rt.mem.write_block(buf, z.data(), kLen);
            w32(buf, 28);                                   // EB_DescribePodsReply
            w32(buf + 4, 1);                                // pod count
            const uint32_t pod = buf + 8;
            w8(pod + 0, 1);                                 // pod_Number
            w8(pod + 1, 1);                                 // pod_Position
            w32(pod + 4, 0x80);                             // pod_Type: control pad
            w32(pod + 8, 16);                               // pod_BitsIn
            w32(pod + 16, 0x80000000u);                     // POD_IsControlPad / generic control pad #0
            w8(pod + 20, 1);                                // pod_GenericNumber[GENERIC_ControlPad] = first pad
            w32(msg.addr + message::DataSize, kLen);
        } else {
            result = int32_t(0x81010108);                   // no room for the description (buffer too small)
        }
    } else {
        stub("event broker flavor " + std::to_string(flavor));
    }
    // Reply to the sender with the result, like ReplyMsg.
    w32(msg.addr + message::Result, uint32_t(result));
    if (HleItem* port = item(reply_to)) send_message(*port, msg);
    return true;
}

static constexpr uint32_t kEvGivingFocus = 19;   // EVENTNUM_GivingFocus

void Portfolio::broker_post(Listener& l, uint32_t pad) {
    if (!item(l.port)) return;
    const uint32_t wants = l.trigger | l.capture;
    const uint32_t pressed = pad & ~last_pad_, released = last_pad_ & ~pad;
    static const bool old_events = std::getenv("TDORT_OLD_PAD_EVENTS") != nullptr;
    std::vector<uint32_t> events;
    // "Giving focus" (event 19) once when the listener starts: games that ask
    // for it (Slam 'n Jam) ignore the pad until they have the focus.
    if ((wants & ev_bit(kEvGivingFocus)) && l.fresh) events.push_back(kEvGivingFocus);
    // "Control button data arrived" is sent on every poll of the pads, not
    // only on changes: Slam 'n Jam reads the pad state from these events.
    if (wants & ev_bit(kEvControlArrived)) events.push_back(kEvControlArrived);
    if ((wants & ev_bit(kEvControlPressed)) && pressed) events.push_back(kEvControlPressed);
    if ((wants & ev_bit(kEvControlReleased)) && released) events.push_back(kEvControlReleased);
    if ((wants & ev_bit(kEvControlUpdate)) && (pad != last_pad_ || l.fresh)) events.push_back(kEvControlUpdate);
    l.fresh = false;
    if (events.empty()) return;

    // A listener that is not reading its port (Madden's boot program waits
    // while its front end runs) keeps at most 4 unread events; the rest are
    // dropped for it alone. Otherwise it took the whole pool, and the front
    // end's own listener got no input at the Game Setup screen.
    int outstanding = 0;
    for (auto& [msg, dest] : broker_dest_)
        if (dest == l.port && std::find(broker_free_.begin(), broker_free_.end(), msg) == broker_free_.end()) ++outstanding;
    if (outstanding >= 4) return;

    // Message + buffer from the pool: 8 in flight per listener at most.
    int32_t m;
    if (!broker_free_.empty()) { m = broker_free_.back(); broker_free_.pop_back(); }
    else if (broker_buf_.size() < 8 * std::max<size_t>(1, listeners_.size())) {
        HleItem& msg = new_item(ItemKind::Message, kSubsysKernel, kNodeMessage, message::Size, "", false);
        msg.owner_task = 0;      // the broker's own: not deleted when the task that happened to be running ends
        msg.reply_port = broker_port_;
        w32(msg.addr + message::ReplyPort, uint32_t(broker_port_));
        m = msg.num;
        broker_buf_[m] = rt.hle_alloc(4 + evframe::PadFrameSize * 5);
    } else return;   // game is not consuming events: drop (queue overflow)

    const uint32_t buf = broker_buf_[m];
    w32(buf, kEbEventRecord);
    uint32_t off = 4;
    for (uint32_t ev : events) {
        const uint32_t f = buf + off;
        for (uint32_t k = 0; k < evframe::PadFrameSize; k += 4) w32(f + k, 0);
        w32(f + evframe::ByteCount, evframe::PadFrameSize);
        w32(f + evframe::TimeStamp, frame_no_);
        w8(f + evframe::EventNumber, uint8_t(ev));
        w8(f + evframe::PodNumber, 1);
        w8(f + evframe::PodPosition, 1);
        w8(f + evframe::GenericPosition, 1);
        w8(f + evframe::Trigger, (l.trigger & ev_bit(ev)) ? 1 : 0);
        // As the console's control pad driver: a "pressed" frame holds only the
        // buttons that just went down, a "released" frame only those that just
        // came up; update/arrived frames hold the whole pad. (Corpse Killer
        // clears held buttons from released frames; with the whole pad there
        // its gun kept firing and the crosshair kept moving.)
        uint32_t data = pad;
        if (ev == kEvGivingFocus) data = 0;
        else if (!old_events) {
            if (ev == kEvControlPressed) data = pressed;
            else if (ev == kEvControlReleased) data = released;
        }
        w32(f + evframe::EventData, data);
        off += evframe::PadFrameSize;
    }
    for (uint32_t k = 0; k < evframe::PadFrameSize; k += 4) w32(buf + off + k, 0);   // terminating frame
    off += evframe::PadFrameSize;

    // Look items up only now: creating the message above can grow the item
    // table and move it, so an HleItem pointer taken earlier would dangle.
    HleItem* port = item(l.port);
    HleItem* msg = item(m);
    if (!port || !msg) return;
    w32(msg->addr + message::DataPtr, buf);
    w32(msg->addr + message::DataSize, off);
    w32(msg->addr + message::Result, 0);
    broker_dest_[m] = l.port;
    send_message(*port, *msg);
}

// A listener whose task has ended never replies to its event messages. Left
// in place, every pad change would cost one of the 8 pooled messages, and
// after eight the broker stops delivering to anyone. Alone in the Dark's
// menus ignored the pad that way: the intro movie player had registered
// as a listener and exited. Drop such listeners and take their messages back.
void Portfolio::drop_dead_listeners() {
    for (auto it = listeners_.begin(); it != listeners_.end();) {
        HleItem* port = item(it->port);
        HleItem* owner = port && port->owner_task ? item(port->owner_task) : nullptr;
        const bool dead = !port || !port->alive || (port->owner_task && (!owner || !owner->alive));
        if (!dead) { ++it; continue; }
        int reclaimed = 0;
        for (auto& [m, dest] : broker_dest_) {
            if (dest != it->port) continue;
            if (std::find(broker_free_.begin(), broker_free_.end(), m) != broker_free_.end()) continue;
            if (port) { auto& q = port->queue; q.erase(std::remove(q.begin(), q.end(), m), q.end()); }
            if (HleItem* msg = item(m)) msg->in_port = 0;
            broker_free_.push_back(m);
            ++reclaimed;
        }
        rt.log("event broker: listener port %d dropped (its task ended), %d message(s) reclaimed", it->port, reclaimed);
        it = listeners_.erase(it);
    }
}

void Portfolio::frame(uint32_t host_pad) {
    ++frame_no_;
    if (graphics_base_) w32(graphics_base_ + graffolio::VBLNumber, frame_no_);   // vertical blank count
    for (auto it = phantom_children_.begin(); it != phantom_children_.end();) {
        if (int32_t(frame_no_ - it->end_frame) >= 0) {
            if (HleItem* t = item(it->item)) t->alive = false;
            raise_signal(it->parent, 0x10 /*SIGF_DEADTASK*/);
            it = phantom_children_.erase(it);
        } else ++it;
    }
    for (auto it = timed_signals_.begin(); it != timed_signals_.end();) {
        if (int32_t(audio_ticks() - it->at_ticks) >= 0) { raise_signal(it->task, it->mask); it = timed_signals_.erase(it); }
        else ++it;
    }
    wake_tasks();
    const uint32_t pad = to_3do_pad(host_pad);
    drop_dead_listeners();
    for (auto& l : listeners_) broker_post(l, pad);
    last_pad_ = pad;
}

} // namespace tdort
