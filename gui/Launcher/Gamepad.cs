// Gamepad.cs - minimal XInput polling so the launcher works from the couch
// with an Xbox controller: D-pad / left stick move, A plays, Y refreshes.
using System;
using System.Runtime.InteropServices;

namespace TdoLauncher;

[Flags]
public enum PadButtons : ushort
{
    None = 0,
    Up = 0x0001, Down = 0x0002, Left = 0x0004, Right = 0x0008,
    Start = 0x0010, Back = 0x0020,
    A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
}

public static class Gamepad
{
    [StructLayout(LayoutKind.Sequential)]
    struct XInputGamepad
    {
        public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short ThumbLX, ThumbLY, ThumbRX, ThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct XInputState { public uint PacketNumber; public XInputGamepad Pad; }

    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    static extern uint GetState14(uint user, out XInputState state);

    [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
    static extern uint GetState910(uint user, out XInputState state);

    static bool _use910, _unavailable;
    const short Dead = 16000;

    /// Buttons held on the first connected controller (stick folded into the D-pad).
    public static PadButtons Read()
    {
        if (_unavailable) return PadButtons.None;
        for (uint i = 0; i < 4; ++i)
        {
            XInputState s;
            uint r;
            try { r = _use910 ? GetState910(i, out s) : GetState14(i, out s); }
            catch (DllNotFoundException)
            {
                if (!_use910) { _use910 = true; return Read(); }
                _unavailable = true; return PadButtons.None;
            }
            catch (EntryPointNotFoundException) { _unavailable = true; return PadButtons.None; }
            if (r != 0) continue;   // not connected

            var b = (PadButtons)s.Pad.Buttons;
            if (s.Pad.ThumbLX < -Dead) b |= PadButtons.Left;
            if (s.Pad.ThumbLX > Dead) b |= PadButtons.Right;
            if (s.Pad.ThumbLY > Dead) b |= PadButtons.Up;
            if (s.Pad.ThumbLY < -Dead) b |= PadButtons.Down;
            return b;
        }
        return PadButtons.None;
    }
}
