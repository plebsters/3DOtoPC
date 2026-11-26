// tdort/portfolio_abi.h - numeric facts about the 3DO Portfolio OS ABI.
//
// Every value here was measured, not guessed: structure offsets come from
// compiling the public Portfolio 2.5 SDK headers for big-endian ARM and
// reading back offsetof()/sizeof(); SWI numbers and vector indices come from
// the same headers; task start-up registers come from disassembling the
// SDK's cstartup object. Only the numbers are recorded, under our own names.
#pragma once
#include <cstdint>

namespace tdort::abi {

// ---- SWI groups ------------------------------------------------------------
constexpr uint32_t kKernelSwi   = 0x10000;
constexpr uint32_t kGraphicsSwi = 0x20000;
constexpr uint32_t kFileSwi     = 0x30000;
constexpr uint32_t kAudioSwi    = 0x40000;
constexpr uint32_t kMathSwi     = 0x50000;

// Kernel SWI indices (kKernelSwi + n)
enum KernelSwi : uint32_t {
    kCreateSizedItem = 0, kWaitSignal = 1, kSendSignal = 2, kDeleteItem = 3, kFindItem = 4,
    kOpenItem = 5, kUnlockItem = 6, kLockItem = 7, kCloseItem = 8, kYield = 9, kSetItemPri = 10,
    kAllocMemBlocks = 13, kGetThisMsg = 15, kSendMsg = 16, kReadHwRandom = 17, kReplyMsg = 18,
    kGetMsg = 19, kControlMem = 20, kAllocSignal = 21, kFreeSignal = 22, kSendIO = 24,
    kAbortIO = 25, kSetItemOwner = 28, kSystemScavengeMem = 33, kFindAndOpenItem = 36,
    kDoIO = 37, kSampleSystemTime = 38, kSetExitStatus = 39, kWaitPort = 40, kWaitIO = 41,
};

// File folio SWI indices (kFileSwi + n)
enum FileSwi : uint32_t {
    kOpenDiskFile = 0, kCloseDiskFile = 1, kMountFileSystem = 4, kOpenDiskFileInDir = 5,
    kMountMacFileSystem = 6, kChangeDirectory = 7, kGetDirectory = 8, kCreateFile = 9,
    kDeleteFile = 10, kCreateAlias = 11, kDismountFileSystem = 13,
};

// Graphics SWI indices (kGraphicsSwi + n)
constexpr uint32_t kDrawScreenCelsSwi = 23, kDrawCelsSwi = 39;

// ---- KernelBase vectors (negative word index from KernelBase) --------------
enum KernelVector : int32_t {
    kvRemHead = -1, kvAddHead = -2, kvRemTail = -3, kvAddTail = -4, kvInsertTail = -5,
    kvRemoveNode = -6, kvAllocMem = -7 /*FromMemLists*/, kvFreeMem = -8 /*ToMemLists*/, kvInitList = -9, kvFindNamedNode = -10,
    kvScavengeMem = -11, kvLookupItem = -12, kvMemSet = -13, kvMemCpy = -14, kvGetPageSize = -15,
    kvCheckItem = -16, kvFindMH = -25, kvUSec2Ticks = -18, kvSetNodePri = -19, kvInsertHead = -20,
    kvVFPrintf = -21, kvGetSysErr = -22, kvTicks2TimeVal = -23, kvAllocMemML = -26,
    kvFreeMemML = -27, kvAllocMemList = -28, kvFreeMemList = -29, kvItemOpened = -32,
    kvFindImage = -38, kvGetMemAllocAlignment = -39, kvGetMemTrackSize = -40, kvCheckIO = -41,
    kvIsMemReadable = -42, kvIsMemWritable = -43, kvFindTagArg = -44, kvNextTagArg = -45,
    // Not in the public headers; behaviour inferred from cstartup, which calls
    // -30 before main() and jumps through -37 after main() returns.
    kvStartupInit = -30, kvExit = -37,
};
constexpr uint32_t kKernelVectorCount = 256;   // generous: older SDKs may use more slots

// Graphics folio vectors (negative word index from GraphicsBase)
enum GraphicsVector : int32_t {
    gvMapSprite = -1, gvReadPixel = -2, gvReadVDLColor = -3, gvGetPixelAddress = -4,
    gvWritePixel = -5, gvDrawText16 = -9, gvResetFont = -11, gvCreateScreenGroup = -12,
    gvSetReadAddress = -13, gvResetReadAddress = -14, gvSetClipOrigin = -15, gvEnableVAvg = -16,
    gvDisableVAvg = -17, gvEnableHAvg = -18, gvDisableHAvg = -19, gvSetScreenColor = -20,
    gvResetScreenColors = -21, gvSetScreenColors = -22, gvDeleteScreenGroup = -23, gvSetFGPen = -24,
    gvSetBGPen = -25, gvAddScreenGroup = -26, gvRemoveScreenGroup = -27, gvSetClipWidth = -28,
    gvSetClipHeight = -29, gvMoveTo = -30, gvDrawChar = -32, gvDrawTo = -33, gvFillRect = -34,
    gvSetCurrentFontCCB = -35, gvGetCurrentFont = -36, gvDrawText8 = -37, gvSetCEControl = -38,
    gvDisplayScreen = -40, gvSetVDL = -41, gvSubmitVDL = -42, gvDrawCels = -43,
    gvDrawScreenCels = -44, gvSetCEWatchdog = -45, gvGetFirstDisplayInfo = -46, gvModifyVDL = -47,
    gvQueryGraphics = -48, gvQueryGraphicsList = -49, gvDisplayOverlay = -50, gvSetVBLAttrs = -51,
    gvGetVBLAttrs = -52,
};

// ---- Task start-up registers (from cstartup) -------------------------------
constexpr int kRegArgc = 5, kRegArgv = 6, kRegKernelBase = 7;

// ---- Node / item identifiers -----------------------------------------------
constexpr uint32_t node_id(uint32_t subsys, uint32_t type) { return (subsys << 8) | type; }
constexpr uint8_t kSubsysKernel = 1, kSubsysGraphics = 2, kSubsysFile = 3, kSubsysAudio = 4, kSubsysMath = 5;
// kernel node types
constexpr uint8_t kNodeList = 2, kNodeFolio = 4, kNodeTask = 5, kNodeSemaphore = 7, kNodeMessage = 9,
                  kNodeMsgPort = 10, kNodeIOReq = 14, kNodeDevice = 15, kNodeTimer = 16;
// file node types
constexpr uint8_t kNodeFileSystem = 1, kNodeFile = 2;
// graphics node types
constexpr uint8_t kNodeScreenGroup = 1, kNodeScreen = 2, kNodeBitmap = 3, kNodeVdl = 4;

// Tags
constexpr uint32_t kTagEnd = 0, kTagJump = 254, kTagNop = 255;
constexpr uint32_t kTagItemName = 1, kTagItemPri = 2;
constexpr uint32_t kTagIOReqReplyPort = 10, kTagIOReqDevice = 11;
constexpr uint32_t kTagPortSignal = 10;
constexpr uint32_t kTagMsgReplyPort = 10, kTagMsgIsSmall = 11, kTagMsgDataSize = 12;   // CREATEMSG_TAG_REPLYPORT, _MSG_IS_SMALL, _DATA_SIZE
// CreateScreenGroup tags
constexpr uint32_t kCsgDisplayHeight = 1, kCsgScreenCount = 2, kCsgScreenHeight = 3,
                   kCsgBitmapCount = 4, kCsgBitmapWidthArray = 5, kCsgBitmapHeightArray = 6,
                   kCsgBitmapBufArray = 7;

// Signals
constexpr uint32_t kSigAbort = 0x04, kSigIODone = 0x08, kSigFirstUser = 8;  // bits 0-7 are system

// Memory type flags
constexpr uint32_t kMemFillMask = 0xFF, kMemFill = 0x100, kMemFromTop = 0x4000, kMemVram = 0x10000, kMemTrackSize = 0x400000;

// IO
constexpr uint8_t kCmdWrite = 0, kCmdRead = 1, kCmdStatus = 2;
constexpr uint8_t kFileCmdReadDir = 3, kFileCmdGetPath = 4, kFileCmdReadEntry = 5;
constexpr uint8_t kTimerCmdDelay = 3, kTimerCmdDelayUntil = 4, kTimerCmdMetronome = 5;
constexpr uint8_t kTimerUnitVblank = 0, kTimerUnitUsec = 1;
constexpr uint32_t kIoDone = 1, kIoQuick = 2;

// ---- Structure layouts (byte offsets) ---------------------------------------
namespace node {   // Node / ItemNode
constexpr uint32_t Next = 0, Prev = 4, SubsysType = 8, Type = 9, Priority = 10, Flags = 11,
                   Size = 12, Name = 16, Version = 20, Revision = 21, FolioFlags = 22,
                   ItemFlags = 23, Item = 24, Owner = 28, ItemNodeSize = 36;
}
namespace list {   // List: a Node followed by an anchor {head, null, tailpred}
constexpr uint32_t Head = 20, Null = 24, TailPred = 28, Size = 32;
}
namespace folio { constexpr uint32_t OpenCount = 36, Size = 112; }
namespace task {
constexpr uint32_t ThreadTask = 36,   // t_ThreadTask: the task that owns a thread (0 for a task)
                   WaitBits = 48, SigBits = 52, AllocatedSigs = 56, StackBase = 60, StackSize = 64,
                   FreeMemoryLists = 168, Size = 220;
}
namespace kbase {
constexpr uint32_t MemFreeLists = 116, MemHdrList = 120, FolioList = 124, Devices = 132, CurrentTask = 152, CPUFlags = 180,
                   BootVolumeName = 272, Tasks = 276, Size = 280;
}
namespace graffolio {   // public part of the Graphics folio structure
constexpr uint32_t Flags = 112, VBLNumber = 116, ZeroPage = 120, VIRSPage = 124, VRAMPageSize = 128,
                   DefaultDisplayWidth = 132, DefaultDisplayHeight = 136, VBLTime = 196, VBLFreq = 200, Size = 332;
}
namespace memhdr {   // one per memory bank (DRAM, VRAM)
constexpr uint32_t Types = 20, PageSize = 24, PageMask = 28, VramPageSize = 32, VramPageMask = 36,
                   FreePageBits = 40, MemBase = 44, MemTop = 48, FreePageBitsSize = 52, PageShift = 53,
                   VramPageShift = 54, Size = 56;
}
// Memory-type flags each bank satisfies (as games probe for them)
constexpr uint32_t kMemTypesDram = 0x3E0000;   // DRAM | DMA | CEL | AUDIO | DSP
constexpr uint32_t kMemTypesVram = 0x370000;   // VRAM | DMA | CEL | AUDIO | DSP
namespace msgport { constexpr uint32_t Signal = 36, Msgs = 40, UserData = 72, Size = 80; }
namespace message {
constexpr uint32_t ReplyPort = 36, Result = 40, DataPtr = 44, DataSize = 48, MsgPort = 52,
                   DataPtrSize = 56, SigItem = 60, Size = 68;
}
namespace ioinfo {
constexpr uint32_t Command = 0, Flags = 1, Unit = 2, Flags2 = 3, CmdOptions = 4, User = 8,
                   Offset = 12, SendBuf = 16, SendLen = 20, RecvBuf = 24, RecvLen = 28, Size = 32;
}
namespace ioreq {
constexpr uint32_t Link = 36, Dev = 44, CallBack = 48, Info = 52, Actual = 84, Flags = 88,
                   Error = 92, Extension = 96, MsgItem = 104, Size = 112;
}
namespace device { constexpr uint32_t Size = 112; }
namespace devstatus {
constexpr uint32_t DriverIdentity = 0, MaxStatusSize = 4, BlockSize = 8, BlockCount = 12,
                   FlagWord = 16, UsageFlags = 20, Size = 36;
}
namespace filestatus { constexpr uint32_t ByteCount = 36, Size = 40; }
namespace direntry {
constexpr uint32_t Flags = 0, UniqueId = 4, Type = 8, BlockSize = 12, ByteCount = 16,
                   BlockCount = 20, Burst = 24, Gap = 28, AvatarCount = 32, FileName = 36,
                   FileNameLen = 32, Location = 68, Size = 72;
}
namespace bitmap {
constexpr uint32_t Buffer = 36, Width = 40, Height = 44, VerticalOffset = 48, Flags = 52,
                   ClipWidth = 56, ClipHeight = 60, ClipX = 64, ClipY = 68, CEControl = 112,
                   Size = 132;
}
namespace screen {
constexpr uint32_t ScreenGroupPtr = 36, VdlPtr = 40, VdlItem = 44, VdlType = 48, BitmapCount = 52,
                   BitmapList = 56, TempBitmap = 120, Size = 124;
}
namespace ccb {
constexpr uint32_t Flags = 0, NextPtr = 4, SourcePtr = 8, PlutPtr = 12, XPos = 16, YPos = 20,
                   HDX = 24, HDY = 28, VDX = 32, VDY = 36, HDDX = 40, HDDY = 44, PIXC = 48,
                   PRE0 = 52, PRE1 = 56, Width = 60, Height = 64, Size = 68;
}

// ---- Event broker (input) ----------------------------------------------------
// Flavors (first word of every message sent to / from the "eventbroker" port)
constexpr uint32_t kEbConfigure = 1, kEbConfigureReply = 2, kEbEventRecord = 3, kEbEventReply = 4;
// Event numbers; in trigger/capture mask word 0, event n is bit (0x80000000 >> (n-1))
constexpr uint32_t kEvControlPressed = 1, kEvControlReleased = 2, kEvControlUpdate = 3, kEvControlArrived = 4;
constexpr uint32_t ev_bit(uint32_t n) { return 0x80000000u >> (n - 1); }
// Control pad button bits (left-justified in the event data word)
constexpr uint32_t kCtlDown = 0x80000000, kCtlUp = 0x40000000, kCtlRight = 0x20000000, kCtlLeft = 0x10000000,
                   kCtlA = 0x08000000, kCtlB = 0x04000000, kCtlC = 0x02000000, kCtlStart = 0x01000000,
                   kCtlX = 0x00800000, kCtlRightShift = 0x00400000, kCtlLeftShift = 0x00200000;
namespace evconfig { constexpr uint32_t Category = 4, TriggerMask = 8, CaptureMask = 40, QueueMax = 72, Size = 108; }
namespace evframe {
constexpr uint32_t ByteCount = 0, SystemID = 4, TimeStamp = 8, Submitter = 12, EventNumber = 16, PodNumber = 17,
                   PodPosition = 18, GenericPosition = 19, Trigger = 20, EventData = 28, PadFrameSize = 32;
}

// Errors are negative. Our HLE uses one generic code per class.
constexpr int32_t kErrBadItem = int32_t(0x81010101), kErrNoMem = int32_t(0x81010102),
                  kErrNotFound = int32_t(0x81010103),
                  // the file folio's "no such file" (MakeFErr(..., ER_Fs_NoFile)); games compare with it
                  kErrFsNoFile = int32_t(0xD556F101), kErrGrafBadClip = int32_t(0xD55B9115), kErrBadPtr = int32_t(0x81010104),
                  kErrNotSupported = int32_t(0x81010105), kErrBadCommand = int32_t(0x81010106),
                  kErrNoSignals = int32_t(0x81010107), kErrEof = int32_t(0x81010108),
                  kErrBadPriv = int32_t(0x81010109),   // not the item's owner
                  kErrIllegalSignal = int32_t(0x8101010A);   // a signal the task didn't allocate

} // namespace tdort::abi
