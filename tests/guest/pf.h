/* pf.h - minimal hand-written Portfolio bindings for test programs.
 * Written from the numeric ABI in runtime/include/tdort/portfolio_abi.h;
 * no SDK headers are used. */
#ifndef PF_H
#define PF_H

typedef unsigned int   u32;
typedef int            s32;
typedef unsigned char  u8;
typedef unsigned short u16;
typedef short s16;
typedef s32 Item;

typedef struct PNode {
    struct PNode* next; struct PNode* prev;
    u8 subsys, type, pri, flags;
    s32 size;
    const char* name;
} PNode;

typedef struct PList { PNode l; PNode* head; PNode* null; PNode* tailpred; } PList;

typedef struct IOInfo {
    u8 cmd, flags, unit, flags2;
    u32 cmd_options, user;
    s32 offset;
    void* send_buf; s32 send_len;
    void* recv_buf; s32 recv_len;
} IOInfo;

typedef struct FileStatus {
    u32 driver_id, max_status_size, block_size, block_count, flag_word, usage_flags;
    u32 pad[3];
    u32 byte_count;
} FileStatus;

typedef struct DirEntry {
    u32 flags, id; char type[4];
    u32 block_size, byte_count, block_count, burst, gap, avatar_count;
    char name[32];
    u32 location;
} DirEntry;

typedef struct TagArg { u32 tag; u32 arg; } TagArg;

extern void* KernelBase;

#define NODE_ID(s, t) (((s) << 8) | (t))
#define CMD_READ   1
#define CMD_STATUS 2
#define FILECMD_READDIR 3
#define FILECMD_GETPATH 4
#define MEMTYPE_FILL 0x100

/* SWIs: r0-r3 in, r0 out */
#define SWI_FN(ret, name, num, params) \
    __attribute__((naked, noinline)) static ret name params { \
        __asm__ volatile("svc #" #num "\n\tmov pc, lr"); }

SWI_FN(Item, CreateSizedItem, 0x10000, (s32 type, TagArg* tags, s32 size))
SWI_FN(s32,  WaitSignal,      0x10001, (u32 mask))
SWI_FN(s32,  DeleteItem,      0x10003, (Item i))
SWI_FN(s32,  AllocSignal,     0x10015, (u32 mask))
SWI_FN(Item, FindAndOpenItem, 0x10024, (s32 type, TagArg* tags))
SWI_FN(s32,  DoIO,            0x10025, (Item ior, IOInfo* info))
SWI_FN(u32,  SampleSystemTime,0x10026, (void))
SWI_FN(s32,  SendSignal,      0x10002, (Item task, u32 mask))
SWI_FN(s32,  UnlockSemaphore, 0x10006, (Item s))
SWI_FN(s32,  LockSemaphore,   0x10007, (Item s, u32 flags))
SWI_FN(s32,  SendMsg,         0x10010, (Item port, Item msg, void* data, s32 size))
SWI_FN(s32,  ReplyMsg,        0x10012, (Item msg, s32 result, void* data, s32 size))
SWI_FN(Item, GetMsg,          0x10013, (Item port))
SWI_FN(Item, WaitPort,        0x10028, (Item port, Item msg))
SWI_FN(Item, FindItem,        0x10004, (s32 type, TagArg* tags))
SWI_FN(s32,  DrawCels,        0x20027, (Item bitmap, void* ccb))
SWI_FN(Item, OpenDiskFile,    0x30000, (const char* path))
SWI_FN(s32,  CloseDiskFile,   0x30001, (Item f))
SWI_FN(Item, ChangeDirectory, 0x30007, (const char* path))

/* KernelBase vectors */
#define KV(idx) (((void**)KernelBase)[idx])
/* AllocMem/FreeMem are wrappers over the memory-list vectors, using the
 * current task's free-memory list (KernelBase+152 -> task, task+168 -> list). */
#define CURRENT_MEMLISTS       (*(void**)((char*)*(void**)((char*)KernelBase + 152) + 168))
#define AllocMem(sz, fl)       ((void* (*)(void*, s32, u32))KV(-7))(CURRENT_MEMLISTS, sz, fl)
#define FreeMem(p, sz)         ((void (*)(void*, void*, s32))KV(-8))(CURRENT_MEMLISTS, p, sz)
#define InitList(l, n)         ((void (*)(PList*, const char*))KV(-9))(l, n)
#define AddHead(l, n)          ((void (*)(PList*, PNode*))KV(-2))(l, n)
#define AddTail(l, n)          ((void (*)(PList*, PNode*))KV(-4))(l, n)
#define RemHead(l)             ((PNode* (*)(PList*))KV(-1))(l)
#define RemTail(l)             ((PNode* (*)(PList*))KV(-3))(l)
#define InsertNodeFromTail(l, n) ((void (*)(PList*, PNode*))KV(-5))(l, n)
#define FindNamedNode(l, s)    ((PNode* (*)(PList*, const char*))KV(-10))(l, s)
#define LookupItem(i)          ((void* (*)(Item))KV(-12))(i)
#define FindTagArg(t, tag)     ((TagArg* (*)(TagArg*, u32))KV(-44))(t, tag)


/* Message (for reading the data pointer of a received message) */
#define MSG_DATAPTR(m)   (*(void**)((char*)(m) + 44))
#define MSG_DATASIZE(m)  (*(s32*)((char*)(m) + 48))
#define CURRENT_TASK_ITEM (*(Item*)((char*)*(void**)((char*)KernelBase + 152) + 24))

typedef struct CCB {
    u32 flags; void* next; void* source; void* plut;
    s32 x, y, hdx, hdy, vdx, vdy, hddx, hddy;
    u32 pixc, pre0, pre1;
    s32 width, height;
} CCB;

#endif
