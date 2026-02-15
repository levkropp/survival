/*
 * efi.h — Minimal UEFI type stubs for TCC self-hosting
 *
 * Provides enough UEFI types for TCC to compile the Survival Workstation
 * source code. Does NOT define _EFI_INCLUDE_ — shim.h runs in non-EFI
 * mode and provides all C standard types.
 */
#ifndef _TCC_EFI_STUB_H
#define _TCC_EFI_STUB_H

/* ---- Qualifiers (no-ops) ---- */
#ifndef IN
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#endif

/* ---- Base types ---- */
/* TCC PE/x86_64 uses LLP64 (long=32-bit), so use long long for 64-bit.
   aarch64 LP64 (long=64-bit) also works fine with long long. */
typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef signed char        INT8;
typedef signed short       INT16;
typedef signed int         INT32;
typedef signed long long   INT64;
typedef unsigned long long UINTN;
typedef signed long long   INTN;
typedef char               CHAR8;
typedef unsigned short     CHAR16;
typedef unsigned char      BOOLEAN;
typedef void               VOID;
typedef UINT64             EFI_LBA;
typedef UINT64             EFI_PHYSICAL_ADDRESS;
typedef VOID              *EFI_EVENT;
typedef VOID              *EFI_HANDLE;
typedef UINTN              EFI_STATUS;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef L
#define L
#endif

/* ---- Status codes ---- */
#define EFI_SUCCESS              0UL
#define EFIERR(a)                (0x8000000000000000UL | (a))
#define EFI_ERROR(a)             ((INTN)(a) < 0)
#define EFI_LOAD_ERROR           EFIERR(1)
#define EFI_INVALID_PARAMETER    EFIERR(2)
#define EFI_UNSUPPORTED          EFIERR(3)
#define EFI_BAD_BUFFER_SIZE      EFIERR(4)
#define EFI_BUFFER_TOO_SMALL     EFIERR(5)
#define EFI_NOT_READY            EFIERR(6)
#define EFI_DEVICE_ERROR         EFIERR(7)
#define EFI_WRITE_PROTECTED      EFIERR(8)
#define EFI_OUT_OF_RESOURCES     EFIERR(9)
#define EFI_NOT_FOUND            EFIERR(14)
#define EFI_ACCESS_DENIED        EFIERR(15)

/* ---- GUID ---- */
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ---- Memory types ---- */
typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData,
    EfiBootServicesCode, EfiBootServicesData,
    EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory,
    EfiACPIReclaimMemory, EfiACPIMemoryNVS,
    EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiMaxMemoryType
} EFI_MEMORY_TYPE;

/* ---- Memory descriptor ---- */
typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- Reset types ---- */
typedef enum {
    EfiResetCold, EfiResetWarm, EfiResetShutdown
} EFI_RESET_TYPE;

/* ---- Locate search types ---- */
typedef enum {
    AllHandles, ByRegisterNotify, ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

/* ---- Allocate types ---- */
typedef enum {
    AllocateAnyPages, AllocateMaxAddress, AllocateAddress, MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* ---- EFI_TIME ---- */
typedef struct {
    UINT16 Year;  UINT8 Month; UINT8 Day;
    UINT8 Hour;   UINT8 Minute; UINT8 Second; UINT8 Pad1;
    UINT32 Nanosecond; INT16 TimeZone; UINT8 Daylight; UINT8 Pad2;
} EFI_TIME;

/* ---- Input key ---- */
typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* ---- Table header ---- */
typedef struct {
    UINT64 Signature; UINT32 Revision; UINT32 HeaderSize;
    UINT32 CRC32; UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ================================================================
 * Protocol forward declarations
 * ================================================================ */

/* ---- Simple Text Input ---- */
typedef struct _SIMPLE_INPUT_INTERFACE SIMPLE_INPUT_INTERFACE;
typedef SIMPLE_INPUT_INTERFACE EFI_SIMPLE_TEXT_IN_PROTOCOL;
typedef SIMPLE_INPUT_INTERFACE EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

struct _SIMPLE_INPUT_INTERFACE {
    EFI_STATUS (EFIAPI *Reset)(SIMPLE_INPUT_INTERFACE *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(SIMPLE_INPUT_INTERFACE *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
};

/* ---- Simple Text Input Ex ---- */
typedef UINT8 EFI_KEY_TOGGLE_STATE;

typedef struct {
    UINT32                 KeyShiftState;
    EFI_KEY_TOGGLE_STATE   KeyToggleState;
} EFI_KEY_STATE;

typedef struct {
    EFI_INPUT_KEY          Key;
    EFI_KEY_STATE          KeyState;
} EFI_KEY_DATA;

#define EFI_SHIFT_STATE_VALID     0x80000000
#define EFI_RIGHT_SHIFT_PRESSED   0x00000001
#define EFI_LEFT_SHIFT_PRESSED    0x00000002
#define EFI_RIGHT_CONTROL_PRESSED 0x00000004
#define EFI_LEFT_CONTROL_PRESSED  0x00000008
#define EFI_RIGHT_ALT_PRESSED     0x00000010
#define EFI_LEFT_ALT_PRESSED      0x00000020

typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStrokeEx)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *, EFI_KEY_DATA *);
    EFI_EVENT WaitForKeyEx;
    void *SetState;
    void *RegisterKeyNotify;
    void *UnregisterKeyNotify;
};

#define EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID \
    { 0xdd9e7534, 0x7762, 0x4698, {0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa} }

/* ---- Simple Text Output ---- */
typedef struct _SIMPLE_TEXT_OUTPUT_INTERFACE SIMPLE_TEXT_OUTPUT_INTERFACE;
typedef SIMPLE_TEXT_OUTPUT_INTERFACE EFI_SIMPLE_TEXT_OUT_PROTOCOL;

struct _SIMPLE_TEXT_OUTPUT_INTERFACE {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(SIMPLE_TEXT_OUTPUT_INTERFACE *, CHAR16 *);
    void *TestString; void *QueryMode; void *SetMode;
    void *SetAttribute; void *ClearScreen;
    void *SetCursorPosition; void *EnableCursor; void *Mode;
};

/* ---- Boot Services ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    /* Task Priority */
    void *RaiseTPL; void *RestoreTPL;
    /* Memory */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE,
                                        UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *,
                                       UINTN *, UINTN *, UINT32 *);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, VOID **);
    EFI_STATUS (EFIAPI *FreePool)(VOID *);
    /* Events */
    void *CreateEvent; void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
    void *SignalEvent; void *CloseEvent; void *CheckEvent;
    /* Protocol Handlers */
    void *InstallProtocolInterface; void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, VOID **);
    void *Reserved1;
    void *RegisterProtocolNotify; void *LocateHandle;
    void *LocateDevicePath; void *InstallConfigurationTable;
    /* Image */
    void *LoadImage; void *StartImage; void *Exit;
    void *UnloadImage; void *ExitBootServices;
    /* Misc */
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
    /* DriverSupport */
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE, EFI_HANDLE *,
                                            VOID *, BOOLEAN);
    EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE);
    /* Open/Close Protocol */
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE, EFI_GUID *, VOID **,
                                       EFI_HANDLE, EFI_HANDLE, UINT32);
    void *CloseProtocol; void *OpenProtocolInformation;
    /* Library */
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE,
                                             EFI_GUID *, VOID *,
                                             UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, VOID *, VOID **);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    /* CRC + Misc */
    void *CalculateCrc32; void *CopyMem; void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---- Runtime Services ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime; void *SetTime;
    void *GetWakeupTime; void *SetWakeupTime;
    void *SetVirtualAddressMap; void *ConvertPointer;
    void *GetVariable; void *GetNextVariableName; void *SetVariable;
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(EFI_RESET_TYPE, EFI_STATUS, UINTN, VOID *);
} EFI_RUNTIME_SERVICES;

/* ---- System Table ---- */
typedef struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    SIMPLE_INPUT_INTERFACE *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut;
    EFI_HANDLE StandardErrorHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ================================================================
 * Graphics Output Protocol
 * ================================================================ */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask, PixelBltOnly, PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct { UINT32 RedMask, GreenMask, BlueMask, ReservedMask; } EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version; UINT32 HorizontalResolution; UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode; UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void *QueryMode; void *SetMode; void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ================================================================
 * File System Protocols
 * ================================================================ */

typedef struct _EFI_FILE_HANDLE EFI_FILE_PROTOCOL;
typedef EFI_FILE_PROTOCOL *EFI_FILE_HANDLE;
typedef EFI_FILE_PROTOCOL EFI_FILE;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(
        struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,
        EFI_FILE_HANDLE *);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct _EFI_FILE_HANDLE {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *, EFI_FILE_HANDLE *,
                               CHAR16 *, UINT64, UINT64);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *, UINT64 *);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *, UINT64);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, VOID *);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, VOID *);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *);
    /* Rev2 extensions */
    void *OpenEx; void *ReadEx; void *WriteEx; void *FlushEx;
};

/* File modes and attributes */
#define EFI_FILE_MODE_READ      0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE     0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE    0x8000000000000000ULL
#define EFI_FILE_DIRECTORY      0x0000000000000010ULL

/* File info */
typedef struct {
    UINT64 Size; UINT64 FileSize; UINT64 PhysicalSize;
    EFI_TIME CreateTime; EFI_TIME LastAccessTime; EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

#define EFI_FILE_INFO_ID \
    { 0x9576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

#define SIZE_OF_EFI_FILE_INFO ((UINTN)&(((EFI_FILE_INFO *)0)->FileName))

/* File system info */
typedef struct {
    UINT64 Size;
    BOOLEAN ReadOnly;
    UINT64 VolumeSize;
    UINT64 FreeSpace;
    UINT32 BlockSize;
    CHAR16 VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;

#define EFI_FILE_SYSTEM_INFO_ID \
    { 0x9576e93, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

/* ================================================================
 * Block IO Protocol
 * ================================================================ */

typedef struct {
    UINT32 MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    EFI_LBA LastBlock;
    /* Rev2/3 extensions */
    EFI_LBA LowestAlignedLba;
    UINT32 LogicalBlocksPerPhysicalBlock;
    UINT32 OptimalTransferLengthGranularity;
} EFI_BLOCK_IO_MEDIA;

typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
typedef EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO;

struct _EFI_BLOCK_IO_PROTOCOL {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(EFI_BLOCK_IO_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32,
                                     EFI_LBA, UINTN, VOID *);
    EFI_STATUS (EFIAPI *WriteBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32,
                                      EFI_LBA, UINTN, VOID *);
    EFI_STATUS (EFIAPI *FlushBlocks)(EFI_BLOCK_IO_PROTOCOL *);
};

/* ================================================================
 * Loaded Image Protocol
 * ================================================================ */

typedef struct { UINT8 Type; UINT8 SubType; UINT8 Length[2]; } EFI_DEVICE_PATH;
typedef EFI_DEVICE_PATH EFI_DEVICE_PATH_PROTOCOL;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    struct _EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef EFI_LOADED_IMAGE_PROTOCOL EFI_LOADED_IMAGE;

/* ================================================================
 * Well-known GUIDs
 * ================================================================ */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B} }
#define LOADED_IMAGE_PROTOCOL EFI_LOADED_IMAGE_PROTOCOL_GUID

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a} }

#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

#endif /* _TCC_EFI_STUB_H */
