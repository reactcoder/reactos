/* $Id: registry.c,v 1.35 2000/09/18 09:39:18 jean Exp $
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/cm/registry.c
 * PURPOSE:         Registry functions
 * PROGRAMMERS:     Rex Jolliff
 *                  Matt Pyne
 * UPDATE HISTORY:
 *                  Created 22/05/98
 */

#undef WIN32_LEAN_AND_MEAN
#include <defines.h>
#include <ddk/ntddk.h>
#include <internal/ob.h>
#include <wchar.h>

#define NDEBUG
#include <internal/debug.h>

//#define  PROTO_REG  1  /* Comment out to disable */

/*  -----------------------------------------------------  Typedefs  */

#define ULONG_MAX 0x7fffffff

#define  REG_BLOCK_SIZE  4096
#define  REG_HEAP_BLOCK_DATA_OFFSET  32
#define  REG_INIT_BLOCK_LIST_SIZE  32
#define  REG_INIT_HASH_TABLE_SIZE  32
#define  REG_EXTEND_HASH_TABLE_SIZE  32
#define  REG_VALUE_LIST_BLOCK_MULTIPLE  32
#define  REG_KEY_BLOCK_ID    0x6b6e
#define  REG_HASH_TABLE_BLOCK_ID  0x666c
#define  REG_VALUE_BLOCK_ID  0x6b76
#define  REG_KEY_BLOCK_TYPE  0x20
#define  REG_ROOT_KEY_BLOCK_TYPE  0x2c

#define  REG_ROOT_KEY_NAME  L"\\Registry"
#define  REG_MACHINE_KEY_NAME  L"\\Registry\\Machine"
#define  REG_SYSTEM_KEY_NAME  L"\\Registry\\Machine\\System"
#define  REG_SOFTWARE_KEY_NAME  L"\\Registry\\Machine\\Software"
#define  REG_USERS_KEY_NAME  L"\\Registry\\User"
#define  REG_USER_KEY_NAME  L"\\Registry\\User\\CurrentUser"

#define  SYSTEM_REG_FILE  L"\\SystemRoot\\System32\\Config\\SYSTEM"
#define  SOFTWARE_REG_FILE  L"\\SystemRoot\\System32\\Config\\SOFTWARE"
#define  USER_REG_FILE  L"\\SystemRoot\\System32\\Config\\USER"

#define  KO_MARKED_FOR_DELETE  0x00000001

// BLOCK_OFFSET = offset in file after header block
typedef DWORD  BLOCK_OFFSET;

typedef struct _HEADER_BLOCK
{
  DWORD  BlockId;
  DWORD  Unused1;
  DWORD  Unused2;
  LARGE_INTEGER  DateModified;
  DWORD  Unused3;
  DWORD  Unused4;
  DWORD  Unused5;
  DWORD  Unused6;
  BLOCK_OFFSET  RootKeyBlock;
  DWORD  BlockSize;
  DWORD  Unused7;
  DWORD  Unused8[115];
  DWORD  Checksum;
} HEADER_BLOCK, *PHEADER_BLOCK;

typedef struct _HEAP_BLOCK
{
  DWORD  BlockId;
  BLOCK_OFFSET  BlockOffset;
  DWORD  BlockSize;
} HEAP_BLOCK, *PHEAP_BLOCK;

// each sub_block begin with this struct :
// in a free subblock, higher bit of SubBlockSize is set
typedef struct _FREE_SUB_BLOCK
{
  DWORD  SubBlockSize;
} FREE_SUB_BLOCK, *PFREE_SUB_BLOCK;

typedef struct _KEY_BLOCK
{
  DWORD  SubBlockSize;
  WORD  SubBlockId;
  WORD  Type;
  LARGE_INTEGER  LastWriteTime;
  DWORD UnUsed1;
  BLOCK_OFFSET  ParentKeyOffset;
  DWORD  NumberOfSubKeys;
  DWORD UnUsed2;
  BLOCK_OFFSET  HashTableOffset;
  DWORD UnUsed3;
  DWORD  NumberOfValues;
  BLOCK_OFFSET  ValuesOffset;
  BLOCK_OFFSET  SecurityKeyOffset;
  BLOCK_OFFSET  ClassNameOffset;
  DWORD  Unused4[5];
  WORD  NameSize;
  WORD  ClassSize;
  UCHAR  Name[0]; /* warning : not zero terminated */
} KEY_BLOCK, *PKEY_BLOCK;

// hash record :
// HashValue=four letters of value's name
typedef struct _HASH_RECORD
{
  BLOCK_OFFSET  KeyOffset;
  ULONG  HashValue;
} HASH_RECORD, *PHASH_RECORD;

typedef struct _HASH_TABLE_BLOCK
{
  DWORD  SubBlockSize;
  WORD  SubBlockId;
  WORD  HashTableSize;
  HASH_RECORD  Table[0];
} HASH_TABLE_BLOCK, *PHASH_TABLE_BLOCK;

typedef struct _VALUE_LIST_BLOCK
{
  DWORD  SubBlockSize;
  BLOCK_OFFSET  Values[0];
} VALUE_LIST_BLOCK, *PVALUE_LIST_BLOCK;

typedef struct _VALUE_BLOCK
{
  DWORD  SubBlockSize;
  WORD  SubBlockId;	// "kv"
  WORD  NameSize;	// length of Name
  DWORD  DataSize;	// length of datas in the subblock pinted by DataOffset
  BLOCK_OFFSET  DataOffset;	// datas are here if DataSize <=4
  DWORD  DataType;
  WORD  Flags;
  WORD  Unused1;
//  FIXME : Name is char, not wchar
  WCHAR  Name[0]; /* warning : not zero terminated */
} VALUE_BLOCK, *PVALUE_BLOCK;

typedef struct _IN_MEMORY_BLOCK
{
  DWORD  FileOffset;
  DWORD  BlockSize;
  PVOID *Data;
} IN_MEMORY_BLOCK, *PIN_MEMORY_BLOCK;

typedef struct _REGISTRY_FILE
{
  PWSTR  Filename;
  HANDLE  FileHandle;
  PHEADER_BLOCK  HeaderBlock;
  ULONG  NumberOfBlocks;
  ULONG  BlockListSize;
  PHEAP_BLOCK  *BlockList;

  NTSTATUS  (*Extend)(ULONG NewSize);
  PVOID  (*Flush)(VOID);
} REGISTRY_FILE, *PREGISTRY_FILE;

/*  Type defining the Object Manager Key Object  */
typedef struct _KEY_OBJECT
{
  CSHORT  Type;
  CSHORT  Size;
  
  ULONG  Flags;
  WCHAR  *Name;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock;
  struct _KEY_OBJECT  *NextKey;
  struct _KEY_OBJECT  *SubKey;
} KEY_OBJECT, *PKEY_OBJECT;


/*  -------------------------------------------------  File Statics  */

static POBJECT_TYPE  CmiKeyType = NULL;
static PREGISTRY_FILE  CmiVolatileFile = NULL;
static PKEY_OBJECT  CmiKeyList = NULL;
static KSPIN_LOCK  CmiKeyListLock;
static PREGISTRY_FILE  CmiSystemFile = NULL;

/*  -----------------------------------------  Forward Declarations  */

static NTSTATUS CmiObjectParse(PVOID ParsedObject,
		     PVOID *NextObject,
		     PUNICODE_STRING FullPath,
		     PWSTR *Path,
		     POBJECT_TYPE ObjectType);

static VOID  CmiObjectDelete(PVOID  DeletedObject);
static NTSTATUS  CmiBuildKeyPath(PWSTR  *KeyPath, 
                                 POBJECT_ATTRIBUTES  ObjectAttributes);
static VOID  CmiAddKeyToList(PKEY_OBJECT  NewKey);
static VOID  CmiRemoveKeyFromList(PKEY_OBJECT  NewKey);
static PKEY_OBJECT  CmiScanKeyList(PWSTR  KeyNameBuf);
static PREGISTRY_FILE  CmiCreateRegistry(PWSTR  Filename);
static NTSTATUS  CmiCreateKey(IN PREGISTRY_FILE  RegistryFile,
                              IN PWSTR  KeyNameBuf,
                              OUT PKEY_BLOCK  *KeyBlock,
                              IN ACCESS_MASK DesiredAccess,
                              IN ULONG TitleIndex,
                              IN PUNICODE_STRING Class, 
                              IN ULONG CreateOptions, 
                              OUT PULONG Disposition);
static NTSTATUS  CmiFindKey(IN PREGISTRY_FILE  RegistryFile,
                            IN PWSTR  KeyNameBuf,
                            OUT PKEY_BLOCK  *KeyBlock,
                            IN ACCESS_MASK DesiredAccess,
                            IN ULONG TitleIndex,
                            IN PUNICODE_STRING Class);
static ULONG  CmiGetMaxNameLength(PREGISTRY_FILE  RegistryFile,
                                  PKEY_BLOCK  KeyBlock);
static ULONG  CmiGetMaxClassLength(PREGISTRY_FILE  RegistryFile,
                                   PKEY_BLOCK  KeyBlock);
static ULONG  CmiGetMaxValueNameLength(PREGISTRY_FILE  RegistryFile,
                                       PKEY_BLOCK  KeyBlock);
static ULONG  CmiGetMaxValueDataLength(PREGISTRY_FILE  RegistryFile,
                                       PKEY_BLOCK  KeyBlock);
static NTSTATUS  CmiScanForSubKey(IN PREGISTRY_FILE  RegistryFile, 
                                  IN PKEY_BLOCK  KeyBlock, 
                                  OUT PKEY_BLOCK  *SubKeyBlock,
                                  IN PCHAR  KeyName,
                                  IN ACCESS_MASK  DesiredAccess);
static NTSTATUS  CmiAddSubKey(IN PREGISTRY_FILE  RegistryFile, 
                              IN PKEY_BLOCK  CurKeyBlock,
                              OUT PKEY_BLOCK  *SubKeyBlock,
                              IN PCHAR  NewSubKeyName,
                              IN ULONG  TitleIndex,
                              IN PWSTR  Class, 
                              IN ULONG  CreateOptions);
static NTSTATUS  CmiScanKeyForValue(IN PREGISTRY_FILE  RegistryFile,
                                    IN PKEY_BLOCK  KeyBlock,
                                    IN PWSTR  ValueName,
                                    OUT PVALUE_BLOCK  *ValueBlock);
static NTSTATUS  CmiGetValueFromKeyByIndex(IN PREGISTRY_FILE  RegistryFile,
                                           IN PKEY_BLOCK  KeyBlock,
                                           IN ULONG  Index,
                                           OUT PVALUE_BLOCK  *ValueBlock);
static NTSTATUS  CmiAddValueToKey(IN PREGISTRY_FILE  RegistryFile,
                                  IN PKEY_BLOCK  KeyBlock,
                                  IN PWSTR  ValueNameBuf,
                                  IN ULONG  Type, 
                                  IN PVOID  Data,
                                  IN ULONG  DataSize);
static NTSTATUS  CmiDeleteValueFromKey(IN PREGISTRY_FILE  RegistryFile,
                                       IN PKEY_BLOCK  KeyBlock,
                                       IN PWSTR  ValueName);
static NTSTATUS  CmiAllocateKeyBlock(IN PREGISTRY_FILE  RegistryFile,
                                     OUT PKEY_BLOCK  *KeyBlock,
                                     IN PCHAR  NewSubKeyName,
                                     IN ULONG  TitleIndex,
                                     IN PWSTR  Class,
                                    IN ULONG  CreateOptions);
static NTSTATUS  CmiDestroyKeyBlock(PREGISTRY_FILE  RegistryFile,
                                    PKEY_BLOCK  KeyBlock);
static NTSTATUS  CmiAllocateHashTableBlock(IN PREGISTRY_FILE  RegistryFile,
                                           OUT PHASH_TABLE_BLOCK  *HashBlock,
                                           IN ULONG  HashTableSize);
static PKEY_BLOCK  CmiGetKeyFromHashByIndex(PREGISTRY_FILE RegistryFile,
                                            PHASH_TABLE_BLOCK  HashBlock,
                                            ULONG  Index);
static NTSTATUS  CmiAddKeyToHashTable(PREGISTRY_FILE  RegistryFile,
                                      PHASH_TABLE_BLOCK  HashBlock,
                                      PKEY_BLOCK  NewKeyBlock);
static NTSTATUS  CmiDestroyHashTableBlock(PREGISTRY_FILE  RegistryFile,
                                          PHASH_TABLE_BLOCK  HashBlock);
static NTSTATUS  CmiAllocateValueBlock(IN PREGISTRY_FILE  RegistryFile,
                                       OUT PVALUE_BLOCK  *ValueBlock,
                                       IN PWSTR  ValueNameBuf,
                                       IN ULONG  Type, 
                                       IN PVOID  Data,
                                       IN ULONG  DataSize);
static NTSTATUS  CmiReplaceValueData(IN PREGISTRY_FILE  RegistryFile,
                                     IN PVALUE_BLOCK  ValueBlock,
                                     IN ULONG  Type, 
                                     IN PVOID  Data,
                                     IN ULONG  DataSize);
static NTSTATUS  CmiDestroyValueBlock(PREGISTRY_FILE  RegistryFile,
                                      PVALUE_BLOCK  ValueBlock);
static NTSTATUS  CmiAllocateBlock(PREGISTRY_FILE  RegistryFile,
                                  PVOID  *Block,
                                  ULONG  BlockSize);
static NTSTATUS  CmiDestroyBlock(PREGISTRY_FILE  RegistryFile,
                                 PVOID  Block);
static PVOID  CmiGetBlock(PREGISTRY_FILE  RegistryFile,
                          BLOCK_OFFSET  BlockOffset);
static BLOCK_OFFSET  CmiGetBlockOffset(PREGISTRY_FILE  RegistryFile,
                                       PVOID  Block);
static VOID CmiLockBlock(PREGISTRY_FILE  RegistryFile,
                         PVOID  Block);
static VOID  CmiReleaseBlock(PREGISTRY_FILE  RegistryFile,
                             PVOID  Block);


/*  ---------------------------------------------  Public Interface  */

VOID
CmInitializeRegistry(VOID)
{
  NTSTATUS  Status;
  HANDLE  RootKeyHandle;
  UNICODE_STRING  RootKeyName;
  OBJECT_ATTRIBUTES  ObjectAttributes;
  PKEY_BLOCK  KeyBlock;
  
  /*  Initialize the Key object type  */
  CmiKeyType = ExAllocatePool(NonPagedPool, sizeof(OBJECT_TYPE));
  CmiKeyType->TotalObjects = 0;
  CmiKeyType->TotalHandles = 0;
  CmiKeyType->MaxObjects = ULONG_MAX;
  CmiKeyType->MaxHandles = ULONG_MAX;
  CmiKeyType->PagedPoolCharge = 0;
  CmiKeyType->NonpagedPoolCharge = sizeof(KEY_OBJECT);
  CmiKeyType->Dump = NULL;
  CmiKeyType->Open = NULL;
  CmiKeyType->Close = NULL;
  CmiKeyType->Delete = CmiObjectDelete;
  CmiKeyType->Parse = CmiObjectParse;
  CmiKeyType->Security = NULL;
  CmiKeyType->QueryName = NULL;
  CmiKeyType->OkayToClose = NULL;
  RtlInitUnicodeString(&CmiKeyType->TypeName, L"Key");

  /*  Build the Root Key Object  */
  /*  FIXME: This should be split into two objects, 1 system and 1 user  */
  RtlInitUnicodeString(&RootKeyName, REG_ROOT_KEY_NAME);
  InitializeObjectAttributes(&ObjectAttributes, &RootKeyName, 0, NULL, NULL);
  ObCreateObject(&RootKeyHandle,
                 STANDARD_RIGHTS_REQUIRED,
                 &ObjectAttributes,
                 CmiKeyType);

  KeInitializeSpinLock(&CmiKeyListLock);

  /*  Build volitile registry store  */
  CmiVolatileFile = CmiCreateRegistry(NULL);

  /*  Build system registry store  */
  CmiSystemFile = NULL; // CmiCreateRegistry(SYSTEM_REG_FILE);

  /*  Create initial predefined symbolic links  */
  /* HKEY_LOCAL_MACHINE  */
  Status = CmiCreateKey(CmiVolatileFile,
                        L"Machine",
                        &KeyBlock,
                        KEY_ALL_ACCESS,
                        0,
                        NULL,
                        REG_OPTION_VOLATILE,
                        0);
  if (!NT_SUCCESS(Status))
    {
      return;
    }
  CmiReleaseBlock(CmiVolatileFile, KeyBlock);
  
  /* HKEY_USERS  */
  Status = CmiCreateKey(CmiVolatileFile,
                        L"Users",
                        &KeyBlock,
                        KEY_ALL_ACCESS,
                        0,
                        NULL,
                        REG_OPTION_VOLATILE,
                        0);
  if (!NT_SUCCESS(Status))
    {
      return;
    }
CHECKPOINT;
  CmiReleaseBlock(CmiVolatileFile, KeyBlock);

  /* FIXME: create remaining structure needed for default handles  */
  /* FIXME: load volatile registry data from ROSDTECT  */

}

VOID
CmInitializeRegistry2(VOID)
{
#ifdef PROTO_REG
 OBJECT_ATTRIBUTES  ObjectAttributes;
 PKEY_OBJECT  NewKey;
 HANDLE  KeyHandle;
 UNICODE_STRING  KeyName;
  /* FIXME : delete temporary \Registry\Machine\System */
  /* load the SYSTEM Hive */
  CmiSystemFile = CmiCreateRegistry(SYSTEM_REG_FILE);
  if( CmiSystemFile )
  {
    RtlInitUnicodeString(&KeyName, REG_SYSTEM_KEY_NAME);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName, 0, NULL, NULL);
    NewKey=ObCreateObject(&KeyHandle,
                 STANDARD_RIGHTS_REQUIRED,
                 &ObjectAttributes,
                 CmiKeyType);
    NewKey->RegistryFile = CmiSystemFile;
    NewKey->KeyBlock = CmiGetBlock(CmiSystemFile,32);
DPRINT("root : id = %x\n",NewKey->KeyBlock->SubBlockId);
DPRINT("root : hashOffset = %x\n",NewKey->KeyBlock->HashTableOffset);
DPRINT("root : nom = %6.6s\n",NewKey->KeyBlock->Name);
    NewKey->Flags = 0;
    NewKey->NextKey = NULL;
/* tests :*/
 {
  HANDLE HKey;
  NTSTATUS Status;
  PKEY_BLOCK SubKeyBlock;
      Status = CmiScanForSubKey(CmiSystemFile, 
                                NewKey->KeyBlock, 
                                &SubKeyBlock,
                                L"ControlSet001",
                                KEY_READ);
CHECKPOINT;
if(NT_SUCCESS(Status))
{
 DPRINT("found subkey ,ptr=%x\n",SubKeyBlock);
 DPRINT("  Id=%x\n",SubKeyBlock->SubBlockId);
 DPRINT("  Type=%x\n",SubKeyBlock->Type);
 DPRINT("  parent=%x\n",SubKeyBlock->ParentKeyOffset);
 DPRINT("  name=%x\n",SubKeyBlock->Name);
}
else
{
 DPRINT("not found subkey ControlSet001\n");
}
//RtlInitUnicodeString(&KeyName, L"\\Registry\\Machine\\Software\\Windows");
RtlInitUnicodeString(&KeyName, L"\\Registry\\Machine\\System\\ControlSet001");
  InitializeObjectAttributes(&ObjectAttributes, &KeyName, 0, NULL, NULL);
  Status = NtOpenKey ( &HKey, KEY_READ , &ObjectAttributes);
DPRINT("  NtOpenKey = %x, HKey=%x\n",Status,HKey);
 }
  }
#endif
}

VOID 
CmImportHive(PCHAR  Chunk)
{
  /*  FIXME: implemement this  */
  return; 
}

NTSTATUS 
STDCALL
NtCreateKey (
	OUT	PHANDLE			KeyHandle,
	IN	ACCESS_MASK		DesiredAccess,
	IN	POBJECT_ATTRIBUTES	ObjectAttributes, 
	IN	ULONG			TitleIndex,
	IN	PUNICODE_STRING		Class,
	IN	ULONG			CreateOptions,
	OUT	PULONG			Disposition
	)
{
  PWSTR  KeyNameBuf;
  NTSTATUS  Status;
  PKEY_OBJECT  CurKey, NewKey;
  PREGISTRY_FILE  FileToUse;
  PKEY_BLOCK  KeyBlock;

  assert(ObjectAttributes != NULL);

  FileToUse = (CreateOptions & REG_OPTION_VOLATILE) ? 
    CmiVolatileFile : CmiSystemFile;
  
  /*  Construct the complete registry relative pathname  */
  Status = CmiBuildKeyPath(&KeyNameBuf, ObjectAttributes);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Scan the key list to see if key already open  */
  CurKey = CmiScanKeyList(KeyNameBuf);
  if (CurKey != NULL)
    {
      /*  Unmark the key if the key has been marked for Delete  */
      if (CurKey->Flags & KO_MARKED_FOR_DELETE)
        {
          CurKey->Flags &= ~KO_MARKED_FOR_DELETE;
        }
      
      /*  If so, return a reference to it  */
      Status = ObCreateHandle(PsGetCurrentProcess(),
                              CurKey,
                              DesiredAccess,
                              FALSE,
                              KeyHandle);
      ExFreePool(KeyNameBuf);
      if (NT_SUCCESS(Status))
        {
          *Disposition = REG_OPENED_EXISTING_KEY;
        }
      return  Status;
    }

  /*  Create or open the key in the registry file  */
  Status = CmiCreateKey(FileToUse,
                        KeyNameBuf,
                        &KeyBlock,
                        DesiredAccess,
                        TitleIndex, 
                        Class, 
                        CreateOptions, 
                        Disposition);
  if (!NT_SUCCESS(Status))
    {
      ExFreePool(KeyNameBuf);
      
      return  Status;
    }

  /*  Create new key object and put into linked list  */
  NewKey = ObCreateObject(KeyHandle, 
                          DesiredAccess, 
                          NULL, 
                          CmiKeyType);
  if (NewKey == NULL)
    {
      return  STATUS_UNSUCCESSFUL;
    }
  NewKey->Flags = 0;
  NewKey->Name = KeyNameBuf;
  NewKey->KeyBlock = KeyBlock;
  NewKey->RegistryFile = FileToUse;
  CmiAddKeyToList(NewKey);
  Status = ObCreateHandle(PsGetCurrentProcess(),
                          NewKey,
                          DesiredAccess,
                          FALSE,
                          KeyHandle);

  if (NT_SUCCESS(Status))
    {
      *Disposition = REG_CREATED_NEW_KEY;
    }
  return  Status;
}


NTSTATUS 
STDCALL
NtDeleteKey (
	IN	HANDLE	KeyHandle
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  
  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_WRITE,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }
  
  /*  Set the marked for delete bit in the key object  */
  KeyObject->Flags |= KO_MARKED_FOR_DELETE;
  
  /*  Dereference the object  */
  ObDeleteHandle(PsGetCurrentProcess(),KeyHandle);
  /* FIXME: I think that ObDeleteHandle should dereference the object  */
  ObDereferenceObject(KeyObject);

  return  STATUS_SUCCESS;
}


NTSTATUS 
STDCALL
NtEnumerateKey (
	IN	HANDLE			KeyHandle,
	IN	ULONG			Index,
	IN	KEY_INFORMATION_CLASS	KeyInformationClass,
	OUT	PVOID			KeyInformation,
	IN	ULONG			Length,
	OUT	PULONG			ResultLength
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock, SubKeyBlock;
  PHASH_TABLE_BLOCK  HashTableBlock;
  PKEY_BASIC_INFORMATION  BasicInformation;
  PKEY_NODE_INFORMATION  NodeInformation;
  PKEY_FULL_INFORMATION  FullInformation;
    
  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_ENUMERATE_SUB_KEYS,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Get pointer to KeyBlock  */
  KeyBlock = KeyObject->KeyBlock;
  RegistryFile = KeyObject->RegistryFile;
    
  /*  Get pointer to SubKey  */
  HashTableBlock = CmiGetBlock(RegistryFile, KeyBlock->HashTableOffset);
  SubKeyBlock = CmiGetKeyFromHashByIndex(RegistryFile, 
                                         HashTableBlock, 
                                         Index);
  if (SubKeyBlock == NULL)
    {
      return  STATUS_NO_MORE_ENTRIES;
    }

  Status = STATUS_SUCCESS;
  switch (KeyInformationClass)
    {
    case KeyBasicInformation:
      /*  Check size of buffer  */
      if (Length < sizeof(KEY_BASIC_INFORMATION) + 
          (SubKeyBlock->NameSize + 1) * sizeof(WCHAR))
        {
          Status = STATUS_BUFFER_OVERFLOW;
        }
      else
        {
          /*  Fill buffer with requested info  */
          BasicInformation = (PKEY_BASIC_INFORMATION) KeyInformation;
          BasicInformation->LastWriteTime = SubKeyBlock->LastWriteTime;
          BasicInformation->TitleIndex = Index;
          BasicInformation->NameLength = (SubKeyBlock->NameSize + 1) * sizeof(WCHAR);
          mbstowcs(BasicInformation->Name, 
                  SubKeyBlock->Name, 
                  SubKeyBlock->NameSize);
          BasicInformation->Name[SubKeyBlock->NameSize] = 0;
          *ResultLength = sizeof(KEY_BASIC_INFORMATION) + 
            SubKeyBlock->NameSize * sizeof(WCHAR);
        }
      break;
      
    case KeyNodeInformation:
      /*  Check size of buffer  */
      if (Length < sizeof(KEY_NODE_INFORMATION) +
          (SubKeyBlock->NameSize + 1) * sizeof(WCHAR) +
          (SubKeyBlock->ClassSize + 1) * sizeof(WCHAR))
        {
          Status = STATUS_BUFFER_OVERFLOW;
        }
      else
        {
          /*  Fill buffer with requested info  */
          NodeInformation = (PKEY_NODE_INFORMATION) KeyInformation;
          NodeInformation->LastWriteTime = SubKeyBlock->LastWriteTime;
          NodeInformation->TitleIndex = Index;
          NodeInformation->ClassOffset = sizeof(KEY_NODE_INFORMATION) + 
            SubKeyBlock->NameSize * sizeof(WCHAR);
          NodeInformation->ClassLength = SubKeyBlock->ClassSize;
          NodeInformation->NameLength = (SubKeyBlock->NameSize + 1) * sizeof(WCHAR);
          mbstowcs(NodeInformation->Name, 
                  SubKeyBlock->Name, 
                  SubKeyBlock->NameSize);
          NodeInformation->Name[SubKeyBlock->NameSize] = 0;
          if (SubKeyBlock->ClassSize != 0)
            {
              wcsncpy(NodeInformation->Name + SubKeyBlock->NameSize + 1,
                      (PWSTR)&SubKeyBlock->Name[SubKeyBlock->NameSize + 1],
                      SubKeyBlock->ClassSize);
              NodeInformation->
                Name[SubKeyBlock->NameSize + 1 + SubKeyBlock->ClassSize] = 0;
            }
          *ResultLength = sizeof(KEY_NODE_INFORMATION) +
            SubKeyBlock->NameSize * sizeof(WCHAR) +
            (SubKeyBlock->ClassSize + 1) * sizeof(WCHAR);
        }
      break;
      
    case KeyFullInformation:
      /* FIXME: check size of buffer  */
      if (Length < sizeof(KEY_FULL_INFORMATION) +
          SubKeyBlock->ClassSize * sizeof(WCHAR))
        {
          Status = STATUS_BUFFER_OVERFLOW;
        }
      else
        {
          /* FIXME: fill buffer with requested info  */
          FullInformation = (PKEY_FULL_INFORMATION) KeyInformation;
          FullInformation->LastWriteTime = SubKeyBlock->LastWriteTime;
          FullInformation->TitleIndex = Index;
          FullInformation->ClassOffset = sizeof(KEY_FULL_INFORMATION) - 
            sizeof(WCHAR);
          FullInformation->ClassLength = SubKeyBlock->ClassSize;
          FullInformation->SubKeys = SubKeyBlock->NumberOfSubKeys;
          FullInformation->MaxNameLen = 
            CmiGetMaxNameLength(RegistryFile, SubKeyBlock);
          FullInformation->MaxClassLen = 
            CmiGetMaxClassLength(RegistryFile, SubKeyBlock);
          FullInformation->Values = SubKeyBlock->NumberOfValues;
          FullInformation->MaxValueNameLen = 
            CmiGetMaxValueNameLength(RegistryFile, SubKeyBlock);
          FullInformation->MaxValueDataLen = 
            CmiGetMaxValueDataLength(RegistryFile, SubKeyBlock);
          wcsncpy(FullInformation->Class,
                  (PWSTR)&SubKeyBlock->Name[SubKeyBlock->NameSize + 1],
                  SubKeyBlock->ClassSize);
          FullInformation->Class[SubKeyBlock->ClassSize] = 0;
          *ResultLength = sizeof(KEY_FULL_INFORMATION) +
            SubKeyBlock->ClassSize * sizeof(WCHAR);
        }
      break;
    }
  CmiReleaseBlock(RegistryFile, SubKeyBlock);
  ObDereferenceObject (KeyObject);

  return  Status;
}


NTSTATUS 
STDCALL
NtEnumerateValueKey (
	IN	HANDLE				KeyHandle,
	IN	ULONG				Index,
	IN	KEY_VALUE_INFORMATION_CLASS	KeyValueInformationClass,
	OUT	PVOID				KeyValueInformation,
	IN	ULONG				Length,
	OUT	PULONG				ResultLength
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock;
  PVALUE_BLOCK  ValueBlock;
  PVOID  DataBlock;
  PKEY_VALUE_BASIC_INFORMATION  ValueBasicInformation;
  PKEY_VALUE_PARTIAL_INFORMATION  ValuePartialInformation;
  PKEY_VALUE_FULL_INFORMATION  ValueFullInformation;

  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_QUERY_VALUE,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Get pointer to KeyBlock  */
  KeyBlock = KeyObject->KeyBlock;
  RegistryFile = KeyObject->RegistryFile;
    
  /*  Get Value block of interest  */
  Status = CmiGetValueFromKeyByIndex(RegistryFile,
                                     KeyBlock,
                                     Index,
                                     &ValueBlock);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }
  else if (ValueBlock != NULL)
    {
      switch (KeyValueInformationClass)
        {
        case KeyValueBasicInformation:
          *ResultLength = sizeof(KEY_VALUE_BASIC_INFORMATION) + 
            (ValueBlock->NameSize + 1) * sizeof(WCHAR);
          if (Length < *ResultLength)
            {
              Status = STATUS_BUFFER_OVERFLOW;
            }
          else
            {
              ValueBasicInformation = (PKEY_VALUE_BASIC_INFORMATION) 
                KeyValueInformation;
              ValueBasicInformation->TitleIndex = 0;
              ValueBasicInformation->Type = ValueBlock->DataType;
              ValueBasicInformation->NameLength =
                (ValueBlock->NameSize + 1) * sizeof(WCHAR);
              wcscpy(ValueBasicInformation->Name, ValueBlock->Name);
            }
          break;

        case KeyValuePartialInformation:
          *ResultLength = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 
            ValueBlock->DataSize;
          if (Length < *ResultLength)
            {
              Status = STATUS_BUFFER_OVERFLOW;
            }
          else
            {
              ValuePartialInformation = (PKEY_VALUE_PARTIAL_INFORMATION)
                KeyValueInformation;
              ValuePartialInformation->TitleIndex = 0;
              ValuePartialInformation->Type = ValueBlock->DataType;
              ValuePartialInformation->DataLength = ValueBlock->DataSize;
              DataBlock = CmiGetBlock(RegistryFile, ValueBlock->DataOffset);
              RtlCopyMemory(ValuePartialInformation->Data, 
                            DataBlock, 
                            ValueBlock->DataSize);
              CmiReleaseBlock(RegistryFile, DataBlock);
            }
          break;

        case KeyValueFullInformation:
          *ResultLength = sizeof(KEY_VALUE_FULL_INFORMATION) + 
            (ValueBlock->NameSize + 1) * sizeof(WCHAR) + ValueBlock->DataSize;
          if (Length < *ResultLength)
            {
              Status = STATUS_BUFFER_OVERFLOW;
            }
          else
            {
              ValueFullInformation = (PKEY_VALUE_FULL_INFORMATION) 
                KeyValueInformation;
              ValueFullInformation->TitleIndex = 0;
              ValueFullInformation->Type = ValueBlock->DataType;
              ValueFullInformation->DataOffset = 
                sizeof(KEY_VALUE_FULL_INFORMATION) + 
                ValueBlock->NameSize * sizeof(WCHAR);
              ValueFullInformation->DataLength = ValueBlock->DataSize;
              ValueFullInformation->NameLength =
                (ValueBlock->NameSize + 1) * sizeof(WCHAR);
              wcscpy(ValueFullInformation->Name, ValueBlock->Name);
              DataBlock = CmiGetBlock(RegistryFile, ValueBlock->DataOffset);
              RtlCopyMemory(&ValueFullInformation->Name[ValueBlock->NameSize + 1],
                            DataBlock,
                            ValueBlock->DataSize);
              CmiReleaseBlock(RegistryFile, DataBlock);
            }
          break;
        }
    }
  else
    {
      Status = STATUS_UNSUCCESSFUL;
    }
  ObDereferenceObject(KeyObject);

  return  Status;
}


NTSTATUS 
STDCALL
NtFlushKey (
	IN	HANDLE	KeyHandle
	)
{
  return  STATUS_SUCCESS;
}


NTSTATUS 
STDCALL
NtOpenKey (
	OUT	PHANDLE			KeyHandle, 
	IN	ACCESS_MASK		DesiredAccess,
	IN	POBJECT_ATTRIBUTES	ObjectAttributes
	)
{
  NTSTATUS  Status;
  PWSTR  KeyNameBuf;
  PREGISTRY_FILE  FileToUse;
  PKEY_BLOCK  KeyBlock;
  PKEY_OBJECT  CurKey, NewKey;
  
  /*  Construct the complete registry relative pathname  */
  Status = CmiBuildKeyPath(&KeyNameBuf, ObjectAttributes);
  if (!NT_SUCCESS(Status))
    {
      return Status;
    }

  /*  Scan the key list to see if key already open  */
  CurKey = CmiScanKeyList(KeyNameBuf);
  if (CurKey != NULL)
    {
      /*  Fail if the key has been deleted  */
      if (CurKey->Flags & KO_MARKED_FOR_DELETE)
        {
          ExFreePool(KeyNameBuf);
          
          return STATUS_UNSUCCESSFUL;
        }
      
      /*  If so, return a reference to it  */
      Status = ObCreateHandle(PsGetCurrentProcess(),
                              CurKey,
                              DesiredAccess,
                              FALSE,
                              KeyHandle);
      ExFreePool(KeyNameBuf);

      return  Status;
    }

  /*  Open the key in the registry file  */
  FileToUse = CmiSystemFile;
  Status = CmiFindKey(FileToUse,
                      KeyNameBuf,
                      &KeyBlock,
                      DesiredAccess,
                      0,
                      NULL);
  if (!NT_SUCCESS(Status))
    {
      FileToUse = CmiVolatileFile;
      Status = CmiFindKey(FileToUse,
                          KeyNameBuf,
                          &KeyBlock,
                          DesiredAccess,
                          0,
                          NULL);
      if (!NT_SUCCESS(Status))
        {
          ExFreePool(KeyNameBuf);
      
          return  Status;
        }
    }

  /*  Create new key object and put into linked list  */
  NewKey = ObCreateObject(KeyHandle, 
                          DesiredAccess, 
                          NULL, 
                          CmiKeyType);
  if (NewKey == NULL)
    {
      return  STATUS_UNSUCCESSFUL;
    }
  NewKey->Flags = 0;
  NewKey->Name = KeyNameBuf;
  NewKey->RegistryFile = FileToUse;
  NewKey->KeyBlock = KeyBlock;
  CmiAddKeyToList(NewKey);
  Status = ObCreateHandle(PsGetCurrentProcess(),
                          NewKey,
                          DesiredAccess,
                          FALSE,
                          KeyHandle);
  
  return  Status;
}


NTSTATUS 
STDCALL
NtQueryKey (
	IN	HANDLE			KeyHandle, 
	IN	KEY_INFORMATION_CLASS	KeyInformationClass,
	OUT	PVOID			KeyInformation,
	IN	ULONG			Length,
	OUT	PULONG			ResultLength
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock;
  PKEY_BASIC_INFORMATION  BasicInformation;
  PKEY_NODE_INFORMATION  NodeInformation;
  PKEY_FULL_INFORMATION  FullInformation;
    
  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_READ,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Get pointer to KeyBlock  */
  KeyBlock = KeyObject->KeyBlock;
  RegistryFile = KeyObject->RegistryFile;
    
  Status = STATUS_SUCCESS;
  switch (KeyInformationClass)
    {
    case KeyBasicInformation:
      /*  Check size of buffer  */
      if (Length < sizeof(KEY_BASIC_INFORMATION) + 
          KeyBlock->NameSize * sizeof(WCHAR))
        {
          Status = STATUS_BUFFER_OVERFLOW;
        }
      else
        {
          /*  Fill buffer with requested info  */
          BasicInformation = (PKEY_BASIC_INFORMATION) KeyInformation;
          BasicInformation->LastWriteTime = KeyBlock->LastWriteTime;
          BasicInformation->TitleIndex = 0;
          BasicInformation->NameLength = KeyBlock->NameSize;
          mbstowcs(BasicInformation->Name, 
                  KeyBlock->Name, 
                  KeyBlock->NameSize);
          BasicInformation->Name[KeyBlock->NameSize] = 0;
          *ResultLength = sizeof(KEY_BASIC_INFORMATION) + 
            KeyBlock->NameSize * sizeof(WCHAR);
        }
      break;
      
    case KeyNodeInformation:
      /*  Check size of buffer  */
      if (Length < sizeof(KEY_NODE_INFORMATION) +
          KeyBlock->NameSize * sizeof(WCHAR) +
          (KeyBlock->ClassSize + 1) * sizeof(WCHAR))
        {
          Status = STATUS_BUFFER_OVERFLOW;
        }
      else
        {
          /*  Fill buffer with requested info  */
          NodeInformation = (PKEY_NODE_INFORMATION) KeyInformation;
          NodeInformation->LastWriteTime = KeyBlock->LastWriteTime;
          NodeInformation->TitleIndex = 0;
          NodeInformation->ClassOffset = sizeof(KEY_NODE_INFORMATION) + 
            KeyBlock->NameSize * sizeof(WCHAR);
          NodeInformation->ClassLength = KeyBlock->ClassSize;
          NodeInformation->NameLength = KeyBlock->NameSize;
          mbstowcs(NodeInformation->Name, 
                  KeyBlock->Name, 
                  KeyBlock->NameSize);
          NodeInformation->Name[KeyBlock->NameSize] = 0;
          if (KeyBlock->ClassSize != 0)
            {
              wcsncpy(NodeInformation->Name + KeyBlock->NameSize + 1,
                      (PWSTR)&KeyBlock->Name[KeyBlock->NameSize + 1],
                      KeyBlock->ClassSize);
              NodeInformation->
                Name[KeyBlock->NameSize + 1 + KeyBlock->ClassSize] = 0;
            }
          *ResultLength = sizeof(KEY_NODE_INFORMATION) +
            KeyBlock->NameSize * sizeof(WCHAR) +
            (KeyBlock->ClassSize + 1) * sizeof(WCHAR);
        }
      break;
      
    case KeyFullInformation:
      /*  Check size of buffer  */
      if (Length < sizeof(KEY_FULL_INFORMATION) +
          KeyBlock->ClassSize * sizeof(WCHAR))
        {
          Status = STATUS_BUFFER_OVERFLOW;
        }
      else
        {
          /*  Fill buffer with requested info  */
          FullInformation = (PKEY_FULL_INFORMATION) KeyInformation;
          FullInformation->LastWriteTime = KeyBlock->LastWriteTime;
          FullInformation->TitleIndex = 0;
          FullInformation->ClassOffset = sizeof(KEY_FULL_INFORMATION) - 
            sizeof(WCHAR);
          FullInformation->ClassLength = KeyBlock->ClassSize;
          FullInformation->SubKeys = KeyBlock->NumberOfSubKeys;
          FullInformation->MaxNameLen = 
            CmiGetMaxNameLength(RegistryFile, KeyBlock);
          FullInformation->MaxClassLen = 
            CmiGetMaxClassLength(RegistryFile, KeyBlock);
          FullInformation->Values = KeyBlock->NumberOfValues;
          FullInformation->MaxValueNameLen = 
            CmiGetMaxValueNameLength(RegistryFile, KeyBlock);
          FullInformation->MaxValueDataLen = 
            CmiGetMaxValueDataLength(RegistryFile, KeyBlock);
          wcsncpy(FullInformation->Class,
                  (PWSTR)&KeyBlock->Name[KeyBlock->NameSize + 1],
                  KeyBlock->ClassSize);
          FullInformation->Class[KeyBlock->ClassSize] = 0;
          *ResultLength = sizeof(KEY_FULL_INFORMATION) +
            KeyBlock->ClassSize * sizeof(WCHAR);
        }
      break;
    }
  ObDereferenceObject (KeyObject);

  return  Status;
}


NTSTATUS 
STDCALL
NtQueryValueKey (
	IN	HANDLE				KeyHandle,
	IN	PUNICODE_STRING			ValueName,
	IN	KEY_VALUE_INFORMATION_CLASS	KeyValueInformationClass,
	OUT	PVOID				KeyValueInformation,
	IN	ULONG				Length,
	OUT	PULONG				ResultLength
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock;
  PVALUE_BLOCK  ValueBlock;
  PVOID  DataBlock;
  PKEY_VALUE_BASIC_INFORMATION  ValueBasicInformation;
  PKEY_VALUE_PARTIAL_INFORMATION  ValuePartialInformation;
  PKEY_VALUE_FULL_INFORMATION  ValueFullInformation;

  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_QUERY_VALUE,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Get pointer to KeyBlock  */
  KeyBlock = KeyObject->KeyBlock;
  RegistryFile = KeyObject->RegistryFile;
    
  /*  Get Value block of interest  */
  Status = CmiScanKeyForValue(RegistryFile, 
                              KeyBlock,
                              ValueName->Buffer,
                              &ValueBlock);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }
  else if (ValueBlock != NULL)
    {
      switch (KeyValueInformationClass)
        {
        case KeyValueBasicInformation:
          *ResultLength = sizeof(KEY_VALUE_BASIC_INFORMATION) + 
            ValueBlock->NameSize * sizeof(WCHAR);
          if (Length < *ResultLength)
            {
              Status = STATUS_BUFFER_OVERFLOW;
            }
          else
            {
              ValueBasicInformation = (PKEY_VALUE_BASIC_INFORMATION) 
                KeyValueInformation;
              ValueBasicInformation->TitleIndex = 0;
              ValueBasicInformation->Type = ValueBlock->DataType;
              ValueBasicInformation->NameLength = ValueBlock->NameSize;
              wcscpy(ValueBasicInformation->Name, ValueBlock->Name);
            }
          break;

        case KeyValuePartialInformation:
          *ResultLength = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 
            ValueBlock->DataSize;
          if (Length < *ResultLength)
            {
              Status = STATUS_BUFFER_OVERFLOW;
            }
          else
            {
              ValuePartialInformation = (PKEY_VALUE_PARTIAL_INFORMATION) 
                KeyValueInformation;
              ValuePartialInformation->TitleIndex = 0;
              ValuePartialInformation->Type = ValueBlock->DataType;
              ValuePartialInformation->DataLength = ValueBlock->DataSize;
              DataBlock = CmiGetBlock(RegistryFile, ValueBlock->DataOffset);
              RtlCopyMemory(ValuePartialInformation->Data, 
                            DataBlock, 
                            ValueBlock->DataSize);
              CmiReleaseBlock(RegistryFile, DataBlock);
            }
          break;

        case KeyValueFullInformation:
          *ResultLength = sizeof(KEY_VALUE_FULL_INFORMATION) + 
            ValueBlock->NameSize * sizeof(WCHAR) + ValueBlock->DataSize;
          if (Length < *ResultLength)
            {
              Status = STATUS_BUFFER_OVERFLOW;
            }
          else
            {
              ValueFullInformation = (PKEY_VALUE_FULL_INFORMATION) 
                KeyValueInformation;
              ValueFullInformation->TitleIndex = 0;
              ValueFullInformation->Type = ValueBlock->DataType;
              ValueFullInformation->DataOffset = 
                sizeof(KEY_VALUE_FULL_INFORMATION) + 
                ValueBlock->NameSize * sizeof(WCHAR);
              ValueFullInformation->DataLength = ValueBlock->DataSize;
              ValueFullInformation->NameLength = ValueBlock->NameSize;
              wcscpy(ValueFullInformation->Name, ValueBlock->Name);
              DataBlock = CmiGetBlock(RegistryFile, ValueBlock->DataOffset);
              RtlCopyMemory(&ValueFullInformation->Name[ValueBlock->NameSize + 1], 
                            DataBlock, 
                            ValueBlock->DataSize);
              CmiReleaseBlock(RegistryFile, DataBlock);
            }
          break;
        }
    }
  else
    {
      Status = STATUS_UNSUCCESSFUL;
    }
  ObDereferenceObject(KeyObject);
  
  return  Status;
}


NTSTATUS 
STDCALL
NtSetValueKey (
	IN	HANDLE			KeyHandle, 
	IN	PUNICODE_STRING		ValueName,
	IN	ULONG			TitleIndex,
	IN	ULONG			Type, 
	IN	PVOID			Data,
	IN	ULONG			DataSize
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock;
  PVALUE_BLOCK  ValueBlock;

  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_SET_VALUE,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Get pointer to KeyBlock  */
  KeyBlock = KeyObject->KeyBlock;
  RegistryFile = KeyObject->RegistryFile;
  Status = CmiScanKeyForValue(RegistryFile,
                              KeyBlock,
                              ValueName->Buffer,
                              &ValueBlock);
  if (!NT_SUCCESS(Status))
    {
      ObDereferenceObject (KeyObject);
      return  Status;
    }
  if (ValueBlock == NULL)
    {
      Status =  CmiAddValueToKey(RegistryFile,
                                 KeyBlock,
                                 ValueName->Buffer,
                                 Type,
                                 Data,
                                 DataSize);
    }
  else
    {
      Status = CmiReplaceValueData(RegistryFile,
                                   ValueBlock,
                                   Type,
                                   Data,
                                   DataSize);
    }
  ObDereferenceObject (KeyObject);
  
  return  Status;
}

NTSTATUS
STDCALL
NtDeleteValueKey (
	IN	HANDLE		KeyHandle,
	IN	PUNICODE_STRING	ValueName
	)
{
  NTSTATUS  Status;
  PKEY_OBJECT  KeyObject;
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  KeyBlock;

  /*  Verify that the handle is valid and is a registry key  */
  Status = ObReferenceObjectByHandle(KeyHandle,
                                     KEY_QUERY_VALUE,
                                     CmiKeyType,
                                     UserMode,
                                     (PVOID *)&KeyObject,
                                     NULL);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }

  /*  Get pointer to KeyBlock  */
  KeyBlock = KeyObject->KeyBlock;
  RegistryFile = KeyObject->RegistryFile;
  Status = CmiDeleteValueFromKey(RegistryFile,
                                 KeyBlock,
                                 ValueName->Buffer);
  ObDereferenceObject(KeyObject);

  return  Status;
}

NTSTATUS
STDCALL 
NtLoadKey (
	PHANDLE			KeyHandle,
	OBJECT_ATTRIBUTES	ObjectAttributes
	)
{
  return  NtLoadKey2(KeyHandle,
                     ObjectAttributes,
                     0);
}


NTSTATUS
STDCALL
NtLoadKey2 (
	PHANDLE			KeyHandle,
	OBJECT_ATTRIBUTES	ObjectAttributes,
	ULONG			Unknown3
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtNotifyChangeKey (
	IN	HANDLE			KeyHandle,
	IN	HANDLE			Event,
	IN	PIO_APC_ROUTINE		ApcRoutine		OPTIONAL, 
	IN	PVOID			ApcContext		OPTIONAL, 
	OUT	PIO_STATUS_BLOCK	IoStatusBlock,
	IN	ULONG			CompletionFilter,
	IN	BOOLEAN			Asynchroneous, 
	OUT	PVOID			ChangeBuffer,
	IN	ULONG			Length,
	IN	BOOLEAN			WatchSubtree
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtQueryMultipleValueKey (
	IN	HANDLE		KeyHandle,
	IN	PWVALENT	ListOfValuesToQuery,
	IN	ULONG		NumberOfItems,
	OUT	PVOID		MultipleValueInformation,
	IN	ULONG		Length,
	OUT	PULONG		ReturnLength
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtReplaceKey (
	IN	POBJECT_ATTRIBUTES	ObjectAttributes,
	IN	HANDLE			Key,
	IN	POBJECT_ATTRIBUTES	ReplacedObjectAttributes
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtRestoreKey (
	IN	HANDLE	KeyHandle,
	IN	HANDLE	FileHandle,
	IN	ULONG	RestoreFlags
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtSaveKey (
	IN	HANDLE	KeyHandle,
	IN	HANDLE	FileHandle
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
NtSetInformationKey (
	IN	HANDLE	KeyHandle,
	IN	CINT	KeyInformationClass,
	IN	PVOID	KeyInformation,
	IN	ULONG	KeyInformationLength
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL 
NtUnloadKey (
	HANDLE	KeyHandle
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL 
NtInitializeRegistry (
	BOOLEAN	SetUpBoot
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
RtlCheckRegistryKey (
	IN	ULONG	RelativeTo,
	IN	PWSTR	Path
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
RtlCreateRegistryKey (
	IN	ULONG	RelativeTo,
	IN	PWSTR	Path
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
RtlDeleteRegistryValue (
	IN	ULONG	RelativeTo,
	IN	PWSTR	Path,
	IN	PWSTR	ValueName
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
RtlQueryRegistryValues (
	IN	ULONG				RelativeTo,
	IN	PWSTR				Path,
	IN	PRTL_QUERY_REGISTRY_TABLE	QueryTable,
	IN	PVOID				Context,
	IN	PVOID				Environment
	)
{
	UNIMPLEMENTED;
}


NTSTATUS
STDCALL
RtlWriteRegistryValue (
	IN	ULONG	RelativeTo,
	IN	PWSTR	Path,
	IN	PWSTR	ValueName,
	IN	ULONG	ValueType,
	IN	PVOID	ValueData,
	IN	ULONG	ValueLength
	)
{
	UNIMPLEMENTED;
}

/*  ------------------------------------------  Private Implementation  */


static NTSTATUS CmiObjectParse(PVOID ParsedObject,
		     PVOID *NextObject,
		     PUNICODE_STRING FullPath,
		     PWSTR *Path,
		     POBJECT_TYPE ObjectType)
{
  NTSTATUS  Status;
  /* FIXME: this should be allocated based on the largest subkey name  */
  CHAR  CurKeyName[260];
  PWSTR  Remainder, NextSlash;
  PREGISTRY_FILE  RegistryFile;
  PKEY_OBJECT  NewKeyObject;
  HANDLE  KeyHandle;
  PKEY_BLOCK  CurKeyBlock, SubKeyBlock;

  Status = STATUS_SUCCESS;

  /* FIXME: it should probably get this from ParsedObject  */
  RegistryFile = CmiVolatileFile;

  /*  Scan key object list for key already open  */
  NewKeyObject = CmiScanKeyList((*Path) + 1);
  if (NewKeyObject != NULL)
    {
      /*  Return reference if found  */
      ObReferenceObjectByPointer(NewKeyObject,
                                 STANDARD_RIGHTS_REQUIRED,
                                 NULL,
                                 UserMode);
      *Path = NULL;

//      return  NewKeyObject;
      //FIXME
      return Status;
    }

  CurKeyBlock = CmiGetBlock(RegistryFile, 
                               RegistryFile->HeaderBlock->RootKeyBlock);

  /*  Loop through each key level and find the needed subkey  */
  Remainder = (*Path) + 1;
  while (NT_SUCCESS(Status) && *Remainder != 0)
    {
      NextSlash = wcschr(Remainder, L'\\');

      /*  Copy just the current subkey name to a buffer  */
      if (NextSlash != NULL)
        {
          wcstombs(CurKeyName, Remainder, NextSlash - Remainder);
          CurKeyName[NextSlash - Remainder] = 0;
        }
      else
        {
          wcstombs(CurKeyName, Remainder, wcslen(Remainder) + 1);
        }

      /* Verify existance of CurKeyName  */
      Status = CmiScanForSubKey(RegistryFile, 
                                CurKeyBlock, 
                                &SubKeyBlock,
                                CurKeyName,
                                STANDARD_RIGHTS_REQUIRED);
      if (!NT_SUCCESS(Status))
        {
          continue;
        }
      if (SubKeyBlock == NULL)
        {
          Status = STATUS_UNSUCCESSFUL;
          continue;
        }
      CmiReleaseBlock(RegistryFile, CurKeyBlock);
      CurKeyBlock = SubKeyBlock;

      if (NextSlash != NULL)
        {
          Remainder = NextSlash + 1;
        }
      else
        {
          Remainder = NULL;
        }
    }
  
  /*  Create new key object and put into linked list  */
  NewKeyObject = ObCreateObject(&KeyHandle, 
                                STANDARD_RIGHTS_REQUIRED, 
                                NULL, 
                                CmiKeyType);
  if (NewKeyObject == NULL)
    {
      //FIXME : return the good error code
      return  STATUS_UNSUCCESSFUL;
    }
  NewKeyObject->Flags = 0;
  NewKeyObject->Name = ExAllocatePool(NonPagedPool, 
                                      wcslen(*Path) * sizeof(WCHAR));
  wcscpy(NewKeyObject->Name, (*Path) + 1);
  NewKeyObject->KeyBlock = CurKeyBlock;
  NewKeyObject->RegistryFile = RegistryFile;
  CmiAddKeyToList(NewKeyObject);
  *Path = (Remainder != NULL) ? Remainder - 1 : NULL;
  
  NextObject = (PVOID)NewKeyObject;
  return STATUS_SUCCESS;
}

static VOID  
CmiObjectDelete(PVOID  DeletedObject)
{
  PKEY_OBJECT  KeyObject;

  KeyObject = (PKEY_OBJECT) DeletedObject;
  if (KeyObject->Flags & KO_MARKED_FOR_DELETE)
    {
      CmiDestroyKeyBlock(KeyObject->RegistryFile,
                         KeyObject->KeyBlock);
    }
  else
    {
      CmiReleaseBlock(KeyObject->RegistryFile,
                      KeyObject->KeyBlock);
    }
  ExFreePool(KeyObject->Name);
  CmiRemoveKeyFromList(KeyObject);
}

static NTSTATUS
CmiBuildKeyPath(PWSTR  *KeyPath, POBJECT_ATTRIBUTES  ObjectAttributes)
{
  NTSTATUS  Status;
  ULONG  KeyNameSize;
  PWSTR  KeyNameBuf;
  PVOID  ObjectBody;
  PKEY_OBJECT  KeyObject;
  POBJECT_HEADER  ObjectHeader;

  /* FIXME: Verify ObjectAttributes is in \\Registry space and compute size for path */
  KeyNameSize = 0;
  ObjectHeader = 0;
  if (ObjectAttributes->RootDirectory != NULL)
    {
      /* FIXME: determine type of object for RootDirectory  */
      Status = ObReferenceObjectByHandle(ObjectAttributes->RootDirectory,
                                         KEY_READ,
                                         NULL,
                                         KernelMode,
                                         (PVOID *)&ObjectBody,
                                         NULL);
      if (!NT_SUCCESS(Status))
        {
          return  Status;
        }
      ObjectHeader = BODY_TO_HEADER(ObjectBody);

      if (ObjectHeader->ObjectType != CmiKeyType)
        {
          /*  Fail if RootDirectory != '\\'  */
          if (ObjectBody == NameSpaceRoot)
            {
              /*  Check for 'Registry' in ObjectName, fail if missing  */
              if (wcsncmp(ObjectAttributes->ObjectName->Buffer, 
                          REG_ROOT_KEY_NAME + 1, 
                          wcslen(REG_ROOT_KEY_NAME + 1)) != 0 ||
                          ObjectAttributes->ObjectName->Buffer[wcslen(REG_ROOT_KEY_NAME + 1)] != L'\\')
                {
                  ObDereferenceObject(ObjectBody);

                  return  STATUS_OBJECT_PATH_INVALID;
                }
 
              /*  Compute size of registry portion of path to KeyNameSize  */
              KeyNameSize = (ObjectAttributes->ObjectName->Length -
                (wcslen(REG_ROOT_KEY_NAME + 1) + 1))
                * sizeof(WCHAR);
            }
          else if (!wcscmp(ObjectHeader->Name.Buffer, 
                           REG_ROOT_KEY_NAME + 1))
            {
              /*  Add size of ObjectName to KeyNameSize  */
              KeyNameSize = ObjectAttributes->ObjectName->Length;
            }
          else 
            {
              ObDereferenceObject(ObjectBody);

              return  STATUS_OBJECT_PATH_INVALID;
            }
        }
      else
        {
          KeyObject = (PKEY_OBJECT) ObjectBody;
        
          /*  Add size of Name from RootDirectory object to KeyNameSize  */
          KeyNameSize = wcslen(KeyObject->Name) * sizeof(WCHAR);

          /*  Add 1 to KeyNamesize for '\\'  */
          KeyNameSize += sizeof(WCHAR);

          /*  Add size of ObjectName to KeyNameSize  */
          KeyNameSize += ObjectAttributes->ObjectName->Length * sizeof(WCHAR);
        }
    }
  else
    {
      /*  Check for \\Registry and fail if missing  */
      if (wcsncmp(ObjectAttributes->ObjectName->Buffer, 
                  REG_ROOT_KEY_NAME, 
                  wcslen(REG_ROOT_KEY_NAME)) != 0 ||
          ObjectAttributes->ObjectName->Buffer[wcslen(REG_ROOT_KEY_NAME)] != L'\\')
        {
          return  STATUS_OBJECT_PATH_INVALID;
        }
 
      /*  Compute size of registry portion of path to KeyNameSize  */
      KeyNameSize = (ObjectAttributes->ObjectName->Length - 
        (wcslen(REG_ROOT_KEY_NAME) + 1)) * sizeof(WCHAR);
    }

  KeyNameBuf = ExAllocatePool(NonPagedPool, KeyNameSize + sizeof(WCHAR));

  /*  Construct relative pathname  */
  KeyNameBuf[0] = 0;
  if (ObjectAttributes->RootDirectory != NULL)
    {
      if (ObjectHeader->ObjectType != CmiKeyType)
        {
          /*  Fail if RootDirectory != '\\'  */
          if (ObjectBody == NameSpaceRoot)
            {
              /*  Copy remainder of ObjectName after 'Registry'  */
              wcscpy(KeyNameBuf, ObjectAttributes->ObjectName->Buffer + wcslen(REG_ROOT_KEY_NAME + 1));
            }
          else
            {
              /*  Copy all of ObjectName  */
              wcscpy(KeyNameBuf, ObjectAttributes->ObjectName->Buffer);
            }
        }
      else
        {
          KeyObject = (PKEY_OBJECT) ObjectBody;
        
          /*  Copy Name from RootDirectory object to KeyNameBuf  */
          wcscpy(KeyNameBuf, KeyObject->Name);

          /*  Append '\\' onto KeyNameBuf */
          wcscat(KeyNameBuf, L"\\");

          /*  Append ObjectName onto KeyNameBuf  */
          wcscat(KeyNameBuf, ObjectAttributes->ObjectName->Buffer);
        }
    }
  else
    {
      /*  Copy registry portion of path into KeyNameBuf  */
      wcscpy(KeyNameBuf, ObjectAttributes->ObjectName->Buffer + 
        (wcslen(REG_ROOT_KEY_NAME) + 1));
    }

  *KeyPath = KeyNameBuf;
  return  STATUS_SUCCESS;
}

static VOID
CmiAddKeyToList(PKEY_OBJECT  NewKey)
{
  KIRQL  OldIrql;
  
  KeAcquireSpinLock(&CmiKeyListLock, &OldIrql);
  NewKey->NextKey = CmiKeyList;
  CmiKeyList = NewKey;
  KeReleaseSpinLock(&CmiKeyListLock, OldIrql);
}

static VOID  
CmiRemoveKeyFromList(PKEY_OBJECT  KeyToRemove)
{
  KIRQL  OldIrql;
  PKEY_OBJECT  CurKey;

  KeAcquireSpinLock(&CmiKeyListLock, &OldIrql);
  if (CmiKeyList == KeyToRemove)
    {
      CmiKeyList = CmiKeyList->NextKey;
    }
  else
    {
      CurKey = CmiKeyList;
      while (CurKey != NULL && CurKey->NextKey != KeyToRemove)
        {
          CurKey = CurKey->NextKey;
        }
      if (CurKey != NULL)
        {
          CurKey->NextKey = KeyToRemove->NextKey;
        }
    }
  KeReleaseSpinLock(&CmiKeyListLock, OldIrql);
}

static PKEY_OBJECT
CmiScanKeyList(PWSTR  KeyName)
{
  KIRQL  OldIrql;
  PKEY_OBJECT  CurKey;

  KeAcquireSpinLock(&CmiKeyListLock, &OldIrql);
  CurKey = CmiKeyList;
  while (CurKey != NULL && wcscmp(KeyName, CurKey->Name) != 0)
    {
      CurKey = CurKey->NextKey;
    }
  KeReleaseSpinLock(&CmiKeyListLock, OldIrql);
  
  return CurKey;
}

static PREGISTRY_FILE  
CmiCreateRegistry(PWSTR  Filename)
{
  PREGISTRY_FILE  RegistryFile;
  PKEY_BLOCK  RootKeyBlock;

  RegistryFile = ExAllocatePool(NonPagedPool, sizeof(REGISTRY_FILE));
  if (Filename != NULL)
   {
     UNICODE_STRING TmpFileName;
     OBJECT_ATTRIBUTES  ObjectAttributes;
     NTSTATUS Status;

      /* Duplicate Filename  */
      RegistryFile->Filename = ExAllocatePool(NonPagedPool, MAX_PATH);
      wcscpy(RegistryFile->Filename , Filename);
      /* FIXME:  if file does not exist, create new file  */
      /* else attempt to map the file  */
      RtlInitUnicodeString (&TmpFileName, Filename);
      InitializeObjectAttributes(&ObjectAttributes,
                             &TmpFileName,
                             0,
                             NULL,
                             NULL);
      Status = ZwOpenFile(&RegistryFile->FileHandle,
                      FILE_ALL_ACCESS,
                      &ObjectAttributes,
                      NULL, 0, 0);
      /* FIXME:  if file does not exist, create new file  */
      if( !NT_SUCCESS(Status) )
      {
	DPRINT("registry file not found\n");
	ExFreePool(RegistryFile->Filename);
	RegistryFile->Filename = NULL;
	return NULL;
      }
      RegistryFile->HeaderBlock = (PHEADER_BLOCK) 
        ExAllocatePool(NonPagedPool, sizeof(HEADER_BLOCK));
      Status = ZwReadFile(RegistryFile->FileHandle, 
                      0, 0, 0, 0, 
                      RegistryFile->HeaderBlock, 
                      sizeof(HEADER_BLOCK), 
                      0, 0);
      RegistryFile->BlockListSize = 0;
      RegistryFile->BlockList = NULL;
   }
  else
    {
      RegistryFile->Filename = NULL;
      RegistryFile->FileHandle = NULL;

      RegistryFile->HeaderBlock = (PHEADER_BLOCK) 
        ExAllocatePool(NonPagedPool, sizeof(HEADER_BLOCK));
      RtlZeroMemory(RegistryFile->HeaderBlock, sizeof(HEADER_BLOCK));
      RegistryFile->HeaderBlock->BlockId = 0x66676572;
      RegistryFile->HeaderBlock->DateModified.QuadPart = 0;
      RegistryFile->HeaderBlock->Unused2 = 1;
      RegistryFile->HeaderBlock->Unused3 = 3;
      RegistryFile->HeaderBlock->Unused5 = 1;
      RegistryFile->HeaderBlock->RootKeyBlock = 0;
      RegistryFile->HeaderBlock->BlockSize = REG_BLOCK_SIZE;
      RegistryFile->HeaderBlock->Unused6 = 1;
      RegistryFile->HeaderBlock->Checksum = 0;
      RootKeyBlock = (PKEY_BLOCK) 
        ExAllocatePool(NonPagedPool, sizeof(KEY_BLOCK));
      RtlZeroMemory(RootKeyBlock, sizeof(KEY_BLOCK));
      RootKeyBlock->SubBlockId = REG_KEY_BLOCK_ID;
      RootKeyBlock->Type = REG_ROOT_KEY_BLOCK_TYPE;
      ZwQuerySystemTime((PTIME) &RootKeyBlock->LastWriteTime);
      RootKeyBlock->ParentKeyOffset = 0;
      RootKeyBlock->NumberOfSubKeys = 0;
      RootKeyBlock->HashTableOffset = -1;
      RootKeyBlock->NumberOfValues = 0;
      RootKeyBlock->ValuesOffset = -1;
      RootKeyBlock->SecurityKeyOffset = 0;
      RootKeyBlock->ClassNameOffset = -1;
      RootKeyBlock->NameSize = 0;
      RootKeyBlock->ClassSize = 0;
      RegistryFile->HeaderBlock->RootKeyBlock = (BLOCK_OFFSET) RootKeyBlock;
    }

  return  RegistryFile;
}

static NTSTATUS
CmiCreateKey(IN PREGISTRY_FILE  RegistryFile,
             IN PWSTR  KeyNameBuf,
             OUT PKEY_BLOCK  *KeyBlock,
             IN ACCESS_MASK DesiredAccess,
             IN ULONG TitleIndex,
             IN PUNICODE_STRING Class, 
             IN ULONG CreateOptions, 
             OUT PULONG Disposition)
{
  /* FIXME: this should be allocated based on the largest subkey name  */
  NTSTATUS  Status;
  CHAR  CurKeyName[256];
  PWSTR  ClassName;
  PWSTR  Remainder, NextSlash;
  PKEY_BLOCK  CurKeyBlock, SubKeyBlock;

  /* FIXME:  Should handle search by Class/TitleIndex  */

CHECKPOINT;
  /*  Loop through each key level and find or build the needed subkey  */
  Status = STATUS_SUCCESS;
  /* FIXME: this access of RootKeyBlock should be guarded by spinlock  */
  CurKeyBlock = CmiGetBlock(RegistryFile, 
                               RegistryFile->HeaderBlock->RootKeyBlock);
CHECKPOINT;
  Remainder = KeyNameBuf;
  while (NT_SUCCESS(Status)  &&
         (NextSlash = wcschr(Remainder, L'\\')) != NULL)
    {
      /*  Copy just the current subkey name to a buffer  */
      wcstombs(CurKeyName, Remainder, NextSlash - Remainder);
      CurKeyName[NextSlash - Remainder] = 0;

      /* Verify existance of/Create CurKeyName  */
      Status = CmiScanForSubKey(RegistryFile,
                                CurKeyBlock,
                                &SubKeyBlock,
                                CurKeyName,
                                DesiredAccess);
      if (!NT_SUCCESS(Status))
        {
          continue;
        }
      if (SubKeyBlock == NULL)
        {
          Status = CmiAddSubKey(RegistryFile,
                                CurKeyBlock,
                                &SubKeyBlock,
                                CurKeyName,
                                0,
                                NULL,
                                0);
          if (!NT_SUCCESS(Status))
            {
              continue;
            }
        }
      CmiReleaseBlock(RegistryFile, CurKeyBlock);
      CurKeyBlock = SubKeyBlock;

      Remainder = NextSlash + 1;
    }
  if (NT_SUCCESS(Status))
    {
      Status = CmiScanForSubKey(RegistryFile,
                                CurKeyBlock,
                                &SubKeyBlock,
                                CurKeyName,
                                DesiredAccess);
      if (NT_SUCCESS(Status))
        {
          if (SubKeyBlock == NULL)
            {
              if (Class != NULL)
                {
                  ClassName = ExAllocatePool(NonPagedPool, Class->Length + 1);
                  wcsncpy(ClassName, Class->Buffer, Class->Length);
                  ClassName[Class->Length] = 0;
                }
              else
                {
                  ClassName = NULL;
                }
              wcstombs(CurKeyName, Remainder, wcslen(Remainder)+1 );
              CurKeyName[ wcslen(Remainder)] = 0;
              Status = CmiAddSubKey(RegistryFile,
                                    CurKeyBlock,
                                    &SubKeyBlock,
                                    CurKeyName,
                                    TitleIndex,
                                    ClassName,
                                    CreateOptions);
              if (ClassName != NULL)
                {
                  ExFreePool(ClassName);
                }
              if (NT_SUCCESS(Status) && Disposition != NULL)
                {
                  *Disposition = REG_CREATED_NEW_KEY;
                }
            }
          else if (Disposition != NULL)
            {
              *Disposition = REG_OPENED_EXISTING_KEY;
            }
        }
      *KeyBlock = SubKeyBlock;
    }
  CmiReleaseBlock(RegistryFile, CurKeyBlock);
  
  return  Status;
}

static NTSTATUS  
CmiFindKey(IN PREGISTRY_FILE  RegistryFile,
           IN PWSTR  KeyNameBuf,
           OUT PKEY_BLOCK  *KeyBlock,
           IN ACCESS_MASK DesiredAccess,
           IN ULONG TitleIndex,
           IN PUNICODE_STRING Class)
{
  /* FIXME: this should be allocated based on the largest subkey name  */
  NTSTATUS  Status;
  CHAR  CurKeyName[MAX_PATH];
  PWSTR  Remainder, NextSlash;
  PKEY_BLOCK  CurKeyBlock, SubKeyBlock;

  if (RegistryFile == NULL)
    return STATUS_UNSUCCESSFUL;

  /* FIXME:  Should handle search by Class/TitleIndex  */

  /*  Loop through each key level and find the needed subkey  */
  Status = STATUS_SUCCESS;
  /* FIXME: this access of RootKeyBlock should be guarded by spinlock  */
  CurKeyBlock = CmiGetBlock(RegistryFile, RegistryFile->HeaderBlock->RootKeyBlock);
  Remainder = KeyNameBuf;
  wcstombs(CurKeyName, Remainder, wcslen(Remainder) + 1 );
  while (NT_SUCCESS(Status) &&
         (NextSlash = wcschr(Remainder, L'\\')) != NULL)
    {
      /*  Copy just the current subkey name to a buffer  */
      wcstombs(CurKeyName, Remainder, NextSlash - Remainder);
      CurKeyName[NextSlash - Remainder] = 0;

      /* Verify existance of CurKeyName  */
      Status = CmiScanForSubKey(RegistryFile, 
                                CurKeyBlock, 
                                &SubKeyBlock,
                                CurKeyName,
                                DesiredAccess);
      if (!NT_SUCCESS(Status))
        {
          continue;
        }
      if (SubKeyBlock == NULL)
        {
          Status = STATUS_UNSUCCESSFUL;
          continue;
        }
      CmiReleaseBlock(RegistryFile, CurKeyBlock);
      CurKeyBlock = SubKeyBlock;

      Remainder = NextSlash + 1;
    }
  if (NT_SUCCESS(Status))
    {
      Status = CmiScanForSubKey(RegistryFile, 
                                CurKeyBlock, 
                                &SubKeyBlock,
                                CurKeyName,
                                DesiredAccess);
      if (NT_SUCCESS(Status))
        {
          if (SubKeyBlock == NULL)
            {
              Status = STATUS_UNSUCCESSFUL;
            }
          else
            {
              *KeyBlock = SubKeyBlock;
            }
        }
    }
  CmiReleaseBlock(RegistryFile, CurKeyBlock);
  
  return  Status;
}

static ULONG  
CmiGetMaxNameLength(PREGISTRY_FILE  RegistryFile,
                    PKEY_BLOCK  KeyBlock)
{
  ULONG  Idx, MaxName;
  PHASH_TABLE_BLOCK  HashBlock;
  PKEY_BLOCK  CurSubKeyBlock;

  MaxName = 0;
  HashBlock = CmiGetBlock(RegistryFile, KeyBlock->HashTableOffset);
  if (HashBlock == 0)
    {
      return  0;
    }
  for (Idx = 0; Idx < HashBlock->HashTableSize; Idx++)
    {
      if (HashBlock->Table[Idx].KeyOffset != 0)
        {
          CurSubKeyBlock = CmiGetBlock(RegistryFile,
                                          HashBlock->Table[Idx].KeyOffset);
          if (MaxName < CurSubKeyBlock->NameSize)
            {
              MaxName = CurSubKeyBlock->NameSize;
            }
          CmiReleaseBlock(RegistryFile, CurSubKeyBlock);
        }
    }

  CmiReleaseBlock(RegistryFile, HashBlock);
  
  return  MaxName;
}

static ULONG  
CmiGetMaxClassLength(PREGISTRY_FILE  RegistryFile,
                     PKEY_BLOCK  KeyBlock)
{
  ULONG  Idx, MaxClass;
  PHASH_TABLE_BLOCK  HashBlock;
  PKEY_BLOCK  CurSubKeyBlock;

  MaxClass = 0;
  HashBlock = CmiGetBlock(RegistryFile, KeyBlock->HashTableOffset);
  if (HashBlock == 0)
    {
      return  0;
    }
  for (Idx = 0; Idx < HashBlock->HashTableSize; Idx++)
    {
      if (HashBlock->Table[Idx].KeyOffset != 0)
        {
          CurSubKeyBlock = CmiGetBlock(RegistryFile,
                                          HashBlock->Table[Idx].KeyOffset);
          if (MaxClass < CurSubKeyBlock->ClassSize)
            {
              MaxClass = CurSubKeyBlock->ClassSize;
            }
          CmiReleaseBlock(RegistryFile, CurSubKeyBlock);
        }
    }

  CmiReleaseBlock(RegistryFile, HashBlock);
  
  return  MaxClass;
}

static ULONG  
CmiGetMaxValueNameLength(PREGISTRY_FILE  RegistryFile,
                         PKEY_BLOCK  KeyBlock)
{
  ULONG  Idx, MaxValueName;
  PVALUE_LIST_BLOCK  ValueListBlock;
  PVALUE_BLOCK  CurValueBlock;

  ValueListBlock = CmiGetBlock(RegistryFile, 
                               KeyBlock->ValuesOffset);
  MaxValueName = 0;
  if (ValueListBlock == 0)
    {
      return  0;
    }
  for (Idx = 0; Idx < KeyBlock->NumberOfValues; Idx++)
    {
      CurValueBlock = CmiGetBlock(RegistryFile,
                                  ValueListBlock->Values[Idx]);
      if (CurValueBlock != NULL &&
          MaxValueName < CurValueBlock->NameSize)
        {
          MaxValueName = CurValueBlock->NameSize;
        }
      CmiReleaseBlock(RegistryFile, CurValueBlock);
    }

  CmiReleaseBlock(RegistryFile, ValueListBlock);
  
  return  MaxValueName;
}

static ULONG  
CmiGetMaxValueDataLength(PREGISTRY_FILE  RegistryFile,
                         PKEY_BLOCK  KeyBlock)
{
  ULONG  Idx, MaxValueData;
  PVALUE_LIST_BLOCK  ValueListBlock;
  PVALUE_BLOCK  CurValueBlock;

  ValueListBlock = CmiGetBlock(RegistryFile, 
                               KeyBlock->ValuesOffset);
  MaxValueData = 0;
  if (ValueListBlock == 0)
    {
      return  0;
    }
  for (Idx = 0; Idx < KeyBlock->NumberOfValues; Idx++)
    {
      CurValueBlock = CmiGetBlock(RegistryFile,
                                  ValueListBlock->Values[Idx]);
      if (CurValueBlock != NULL &&
          MaxValueData < CurValueBlock->DataSize)
        {
          MaxValueData = CurValueBlock->DataSize;
        }
      CmiReleaseBlock(RegistryFile, CurValueBlock);
    }

  CmiReleaseBlock(RegistryFile, ValueListBlock);
  
  return  MaxValueData;
}

static NTSTATUS
CmiScanForSubKey(IN PREGISTRY_FILE  RegistryFile, 
                 IN PKEY_BLOCK  KeyBlock, 
                 OUT PKEY_BLOCK  *SubKeyBlock,
                 IN PCHAR  KeyName,
                 IN ACCESS_MASK  DesiredAccess)
{
  ULONG  Idx;
  PHASH_TABLE_BLOCK  HashBlock;
  PKEY_BLOCK  CurSubKeyBlock;
  WORD KeyLength = strlen(KeyName);

  HashBlock = CmiGetBlock(RegistryFile, KeyBlock->HashTableOffset);
  *SubKeyBlock = NULL;
  if (HashBlock == NULL)
    {
      return  STATUS_SUCCESS;
    }
DPRINT("hash=%x,hash.id=%x\n",HashBlock,HashBlock->SubBlockId);
//  for (Idx = 0; Idx < HashBlock->HashTableSize; Idx++)
  for (Idx = 0; Idx < KeyBlock->NumberOfSubKeys; Idx++)
    {
DPRINT("&hash=%x,hash=%4.4s\n",&HashBlock->Table[Idx].HashValue,&HashBlock->Table[Idx].HashValue);
      if (HashBlock->Table[Idx].KeyOffset != 0 &&
           HashBlock->Table[Idx].KeyOffset != -1 &&
          !strncmp(KeyName, (PCHAR) &HashBlock->Table[Idx].HashValue, 4))
        {
CHECKPOINT;
          CurSubKeyBlock = CmiGetBlock(RegistryFile,
                                          HashBlock->Table[Idx].KeyOffset);
          if ( CurSubKeyBlock->NameSize == KeyLength
                && !memcmp(KeyName, CurSubKeyBlock->Name, KeyLength))
            {
              *SubKeyBlock = CurSubKeyBlock;
              break;
            }
          else
            {
              CmiReleaseBlock(RegistryFile, CurSubKeyBlock);
            }
        }
    }
CHECKPOINT;

  CmiReleaseBlock(RegistryFile, HashBlock);
  
  return  STATUS_SUCCESS;
}

static NTSTATUS
CmiAddSubKey(PREGISTRY_FILE  RegistryFile, 
             PKEY_BLOCK  KeyBlock,
             PKEY_BLOCK  *SubKeyBlock,
             PCHAR  NewSubKeyName,
             ULONG  TitleIndex,
             PWSTR  Class, 
             ULONG  CreateOptions)
{
  NTSTATUS  Status;
  PHASH_TABLE_BLOCK  HashBlock, NewHashBlock;
  PKEY_BLOCK  NewKeyBlock;

  Status = CmiAllocateKeyBlock(RegistryFile,
                               &NewKeyBlock,
                               NewSubKeyName,
                               TitleIndex,
                               Class,
                               CreateOptions);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }
  if (KeyBlock->HashTableOffset == -1)
    {
      Status = CmiAllocateHashTableBlock(RegistryFile, 
                                         &HashBlock,
                                         REG_INIT_HASH_TABLE_SIZE);
      if (!NT_SUCCESS(Status))
        {
          return  Status;
        }
      KeyBlock->HashTableOffset = CmiGetBlockOffset(RegistryFile, HashBlock);
    }
  else
    {
      HashBlock = CmiGetBlock(RegistryFile, KeyBlock->HashTableOffset);
      if (KeyBlock->NumberOfSubKeys + 1 >= HashBlock->HashTableSize)
        {

          /* FIXME: All Subkeys will need to be rehashed here!  */

          /*  Reallocate the hash table block  */
          Status = CmiAllocateHashTableBlock(RegistryFile,
                                             &NewHashBlock,
                                             HashBlock->HashTableSize +
                                               REG_EXTEND_HASH_TABLE_SIZE);
          if (!NT_SUCCESS(Status))
            {
              return  Status;
            }
          RtlZeroMemory(&NewHashBlock->Table[0],
                        sizeof(NewHashBlock->Table[0]) * NewHashBlock->HashTableSize);
          RtlCopyMemory(&NewHashBlock->Table[0],
                        &HashBlock->Table[0],
                        sizeof(NewHashBlock->Table[0]) * HashBlock->HashTableSize);
          KeyBlock->HashTableOffset = CmiGetBlockOffset(RegistryFile, NewHashBlock);
          CmiDestroyHashTableBlock(RegistryFile, HashBlock);
          HashBlock = NewHashBlock;
        }
    }
  Status = CmiAddKeyToHashTable(RegistryFile, HashBlock, NewKeyBlock);
  if (NT_SUCCESS(Status))
    {
      KeyBlock->NumberOfSubKeys++;
      *SubKeyBlock = NewKeyBlock;
    }
  CmiReleaseBlock(RegistryFile, HashBlock);
  
  return  Status;
}

static NTSTATUS  
CmiScanKeyForValue(IN PREGISTRY_FILE  RegistryFile,
                   IN PKEY_BLOCK  KeyBlock,
                   IN PWSTR  ValueName,
                   OUT PVALUE_BLOCK  *ValueBlock)
{
  ULONG  Idx;
  PVALUE_LIST_BLOCK  ValueListBlock;
  PVALUE_BLOCK  CurValueBlock;

  ValueListBlock = CmiGetBlock(RegistryFile, 
                               KeyBlock->ValuesOffset);
  *ValueBlock = NULL;
  if (ValueListBlock == NULL)
    {
      return  STATUS_SUCCESS;
    }
  for (Idx = 0; Idx < KeyBlock->NumberOfValues; Idx++)
    {
      CurValueBlock = CmiGetBlock(RegistryFile,
                                  ValueListBlock->Values[Idx]);
      if (CurValueBlock != NULL &&
          !wcscmp(CurValueBlock->Name, ValueName))
        {
          *ValueBlock = CurValueBlock;
          break;
        }
      CmiReleaseBlock(RegistryFile, CurValueBlock);
    }

  CmiReleaseBlock(RegistryFile, ValueListBlock);
  
  return  STATUS_SUCCESS;
}


static NTSTATUS
CmiGetValueFromKeyByIndex(IN PREGISTRY_FILE  RegistryFile,
                          IN PKEY_BLOCK  KeyBlock,
                          IN ULONG  Index,
                          OUT PVALUE_BLOCK  *ValueBlock)
{
  PVALUE_LIST_BLOCK  ValueListBlock;
  PVALUE_BLOCK  CurValueBlock;

  ValueListBlock = CmiGetBlock(RegistryFile,
                               KeyBlock->ValuesOffset);
  *ValueBlock = NULL;
  if (ValueListBlock == NULL)
    {
      return STATUS_NO_MORE_ENTRIES;
    }
  if (Index >= KeyBlock->NumberOfValues)
    {
      return STATUS_NO_MORE_ENTRIES;
    }
  CurValueBlock = CmiGetBlock(RegistryFile,
                              ValueListBlock->Values[Index]);
  if (CurValueBlock != NULL)
    {
      *ValueBlock = CurValueBlock;
    }
  CmiReleaseBlock(RegistryFile, CurValueBlock);
  CmiReleaseBlock(RegistryFile, ValueListBlock);
  
  return  STATUS_SUCCESS;
}

static NTSTATUS  
CmiAddValueToKey(IN PREGISTRY_FILE  RegistryFile,
                 IN PKEY_BLOCK  KeyBlock,
                 IN PWSTR  ValueNameBuf,
                 IN ULONG  Type, 
                 IN PVOID  Data,
                 IN ULONG  DataSize)
{
  NTSTATUS  Status;
  PVALUE_LIST_BLOCK  ValueListBlock, NewValueListBlock;
  PVALUE_BLOCK  ValueBlock;

  Status = CmiAllocateValueBlock(RegistryFile,
                                 &ValueBlock,
                                 ValueNameBuf,
                                 Type,
                                 Data,
                                 DataSize);
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }
  ValueListBlock = CmiGetBlock(RegistryFile, 
                               KeyBlock->ValuesOffset);
  if (ValueListBlock == NULL)
    {
      Status = CmiAllocateBlock(RegistryFile,
                                (PVOID) &ValueListBlock,
                                sizeof(BLOCK_OFFSET) *
                                  REG_VALUE_LIST_BLOCK_MULTIPLE);
      if (!NT_SUCCESS(Status))
        {
          CmiDestroyValueBlock(RegistryFile,
                               ValueBlock);
          return  Status;
        }
      KeyBlock->ValuesOffset = CmiGetBlockOffset(RegistryFile,
                                                 ValueListBlock);
    }
  else if (KeyBlock->NumberOfValues % REG_VALUE_LIST_BLOCK_MULTIPLE)
    {
      Status = CmiAllocateBlock(RegistryFile,
                                (PVOID) &NewValueListBlock,
                                sizeof(BLOCK_OFFSET) *
                                  (KeyBlock->NumberOfValues + 
                                    REG_VALUE_LIST_BLOCK_MULTIPLE));
      if (!NT_SUCCESS(Status))
        {
          CmiDestroyValueBlock(RegistryFile,
                               ValueBlock);
          return  Status;
        }
      RtlCopyMemory(NewValueListBlock, 
                    ValueListBlock,
                    sizeof(BLOCK_OFFSET) * KeyBlock->NumberOfValues);
      KeyBlock->ValuesOffset = CmiGetBlockOffset(RegistryFile, 
                                                 NewValueListBlock);
      CmiDestroyBlock(RegistryFile, ValueListBlock);
      ValueListBlock = NewValueListBlock;
    }
  ValueListBlock->Values[KeyBlock->NumberOfValues] = 
    CmiGetBlockOffset(RegistryFile, ValueBlock);
  KeyBlock->NumberOfValues++;
  CmiReleaseBlock(RegistryFile, ValueListBlock);
  CmiReleaseBlock(RegistryFile, ValueBlock);

  return  STATUS_SUCCESS;
}

static NTSTATUS  
CmiDeleteValueFromKey(IN PREGISTRY_FILE  RegistryFile,
                      IN PKEY_BLOCK  KeyBlock,
                      IN PWSTR  ValueName)
{
  ULONG  Idx;
  PVALUE_LIST_BLOCK  ValueListBlock;
  PVALUE_BLOCK  CurValueBlock;

  ValueListBlock = CmiGetBlock(RegistryFile, 
                               KeyBlock->ValuesOffset);
  if (ValueListBlock == 0)
    {
      return  STATUS_SUCCESS;
    }
  for (Idx = 0; Idx < KeyBlock->NumberOfValues; Idx++)
    {
      CurValueBlock = CmiGetBlock(RegistryFile,
                                  ValueListBlock->Values[Idx]);
      if (CurValueBlock != NULL &&
          !wcscmp(CurValueBlock->Name, ValueName))
        {
          if (KeyBlock->NumberOfValues - 1 < Idx)
            {
              RtlCopyMemory(&ValueListBlock->Values[Idx],
                            &ValueListBlock->Values[Idx + 1],
                            sizeof(BLOCK_OFFSET) * 
                              (KeyBlock->NumberOfValues - 1 - Idx));
            }
          else
            {
              RtlZeroMemory(&ValueListBlock->Values[Idx],
                            sizeof(BLOCK_OFFSET));
            }
          KeyBlock->NumberOfValues -= 1;
          CmiDestroyValueBlock(RegistryFile, CurValueBlock);

          break;
        }
      CmiReleaseBlock(RegistryFile, CurValueBlock);
    }

  CmiReleaseBlock(RegistryFile, ValueListBlock);
  
  return  STATUS_SUCCESS;
}

static NTSTATUS
CmiAllocateKeyBlock(IN PREGISTRY_FILE  RegistryFile,
                    OUT PKEY_BLOCK  *KeyBlock,
                    IN PCHAR  KeyName,
                    IN ULONG  TitleIndex,
                    IN PWSTR  Class,
                    IN ULONG  CreateOptions)
{
  NTSTATUS  Status;
  ULONG  NewBlockSize;
  PKEY_BLOCK  NewKeyBlock;

  DPRINT("RegistryFile %p KeyBlock %p KeyName %s TitleIndex %x Class %S CreateOptions %x\n",
         RegistryFile, KeyBlock, KeyName, TitleIndex, Class,CreateOptions);

  Status = STATUS_SUCCESS;

      NewBlockSize = sizeof(KEY_BLOCK) + (strlen(KeyName) ) ;
DPRINT ("NewKeySize: %lu\n", NewBlockSize);
//CHECKPOINT;
      Status = CmiAllocateBlock(RegistryFile, (PVOID) &NewKeyBlock , NewBlockSize);
//CHECKPOINT;
      if (NewKeyBlock == NULL)
        {
          Status = STATUS_INSUFFICIENT_RESOURCES;
        }
      else
        {
          RtlZeroMemory(NewKeyBlock, NewBlockSize);
          NewKeyBlock->SubBlockId = REG_KEY_BLOCK_ID;
          NewKeyBlock->Type = REG_KEY_BLOCK_TYPE;
          ZwQuerySystemTime((PTIME) &NewKeyBlock->LastWriteTime);
          NewKeyBlock->ParentKeyOffset = -1;
          NewKeyBlock->NumberOfSubKeys = 0;
          NewKeyBlock->HashTableOffset = -1;
          NewKeyBlock->NumberOfValues = 0;
          NewKeyBlock->ValuesOffset = -1;
          NewKeyBlock->SecurityKeyOffset = -1;
          NewKeyBlock->NameSize = strlen(KeyName);
          NewKeyBlock->ClassSize = (Class != NULL) ? wcslen(Class) : 0;
          memcpy(NewKeyBlock->Name, KeyName, NewKeyBlock->NameSize );
          if (Class != NULL)
            {
          /* FIXME : ClassName is in a different Block !!! */
            }
          CmiLockBlock(RegistryFile, NewKeyBlock);
          *KeyBlock = NewKeyBlock;
        }

  return  Status;
}

static NTSTATUS
CmiDestroyKeyBlock(PREGISTRY_FILE  RegistryFile,
                   PKEY_BLOCK  KeyBlock)
{
  NTSTATUS  Status;

  Status = STATUS_SUCCESS;

  if (RegistryFile->Filename == NULL)
    {
      CmiReleaseBlock(RegistryFile, KeyBlock);
      ExFreePool(KeyBlock);
    }
  else
    {  
      UNIMPLEMENTED;
    }

  return  Status;
}

static NTSTATUS
CmiAllocateHashTableBlock(IN PREGISTRY_FILE  RegistryFile,
                          OUT PHASH_TABLE_BLOCK  *HashBlock,
                          IN ULONG  HashTableSize)
{
  NTSTATUS  Status;
  ULONG  NewHashSize;
  PHASH_TABLE_BLOCK  NewHashBlock;

  Status = STATUS_SUCCESS;

  /*  Handle volatile files first  */
  if (RegistryFile->Filename == NULL)
    {
      NewHashSize = sizeof(HASH_TABLE_BLOCK) + 
        (HashTableSize - 1) * sizeof(HASH_RECORD);
      NewHashBlock = ExAllocatePool(NonPagedPool, NewHashSize);
      if (NewHashBlock == NULL)
        {
          Status = STATUS_INSUFFICIENT_RESOURCES;
        }
      else
        {
          RtlZeroMemory(NewHashBlock, NewHashSize);
          NewHashBlock->SubBlockId = REG_HASH_TABLE_BLOCK_ID;
          NewHashBlock->HashTableSize = HashTableSize;
          CmiLockBlock(RegistryFile, NewHashBlock);
          *HashBlock = NewHashBlock;
        }
    }
  else
    {
      UNIMPLEMENTED;
    }

  return  Status;
}

static PKEY_BLOCK  
CmiGetKeyFromHashByIndex(PREGISTRY_FILE RegistryFile,
                         PHASH_TABLE_BLOCK  HashBlock,
                         ULONG  Index)
{
  PKEY_BLOCK  KeyBlock;

  if (RegistryFile->Filename == NULL)
    {
      KeyBlock = (PKEY_BLOCK) HashBlock->Table[Index].KeyOffset;
      CmiLockBlock(RegistryFile, KeyBlock);
    }
  else
    {
      UNIMPLEMENTED;
    }

  return  KeyBlock;
}

static NTSTATUS  
CmiAddKeyToHashTable(PREGISTRY_FILE  RegistryFile,
                     PHASH_TABLE_BLOCK  HashBlock,
                     PKEY_BLOCK  NewKeyBlock)
{
  ULONG i;

  for (i = 0; i < HashBlock->HashTableSize; i++)
    {
       if (HashBlock->Table[i].KeyOffset == 0)
         {
            HashBlock->Table[i].KeyOffset =
              CmiGetBlockOffset(RegistryFile, NewKeyBlock);
            RtlCopyMemory(&HashBlock->Table[i].HashValue,
                          NewKeyBlock->Name,
                          4);
            return STATUS_SUCCESS;
         }
    }
  return STATUS_UNSUCCESSFUL;
}

static NTSTATUS
CmiDestroyHashTableBlock(PREGISTRY_FILE  RegistryFile,
                         PHASH_TABLE_BLOCK  HashBlock)
{
  NTSTATUS  Status;

  Status = STATUS_SUCCESS;

  if (RegistryFile->Filename == NULL)
    {
      CmiReleaseBlock(RegistryFile, HashBlock);
      ExFreePool(HashBlock);
    }
  else
    {  
      Status = STATUS_NOT_IMPLEMENTED;
    }

  return  Status;
}

static NTSTATUS
CmiAllocateValueBlock(PREGISTRY_FILE  RegistryFile,
                      PVALUE_BLOCK  *ValueBlock,
                      IN PWSTR  ValueNameBuf,
                      IN ULONG  Type, 
                      IN PVOID  Data,
                      IN ULONG  DataSize)
{
  NTSTATUS  Status;
  ULONG  NewValueSize;
  PVALUE_BLOCK  NewValueBlock;
  PVOID  DataBlock;

  Status = STATUS_SUCCESS;

  /*  Handle volatile files first  */
  if (RegistryFile->Filename == NULL)
    {
      NewValueSize = sizeof(VALUE_BLOCK) + wcslen(ValueNameBuf)* sizeof(WCHAR);
      NewValueBlock = ExAllocatePool(NonPagedPool, NewValueSize);
      if (NewValueBlock == NULL)
        {
          Status = STATUS_INSUFFICIENT_RESOURCES;
        }
      else
        {
          RtlZeroMemory(NewValueBlock, NewValueSize);
          NewValueBlock->SubBlockId = REG_VALUE_BLOCK_ID;
          NewValueBlock->NameSize = wcslen(ValueNameBuf);
          wcscpy(NewValueBlock->Name, ValueNameBuf);
          NewValueBlock->DataType = Type;
          NewValueBlock->DataSize = DataSize;
          Status = CmiAllocateBlock(RegistryFile,
                                    &DataBlock,
                                    DataSize);
          if (!NT_SUCCESS(Status))
            {
              ExFreePool(NewValueBlock);
            }
          else
            {
              RtlCopyMemory(DataBlock, Data, DataSize);
              NewValueBlock->DataOffset = CmiGetBlockOffset(RegistryFile,
                                                            DataBlock);
              CmiLockBlock(RegistryFile, NewValueBlock);
              CmiReleaseBlock(RegistryFile, DataBlock);
              *ValueBlock = NewValueBlock;
            }
        }
    }
  else
    {
      Status = STATUS_NOT_IMPLEMENTED;
    }

  return  Status;
}

static NTSTATUS  
CmiReplaceValueData(IN PREGISTRY_FILE  RegistryFile,
                    IN PVALUE_BLOCK  ValueBlock,
                    IN ULONG  Type, 
                    IN PVOID  Data,
                    IN ULONG  DataSize)
{
  NTSTATUS  Status;
  PVOID   DataBlock, NewDataBlock;

  Status = STATUS_SUCCESS;

  /* If new data size is <= current then overwrite current data  */
  if (DataSize <= ValueBlock->DataSize)
    {
      DataBlock = CmiGetBlock(RegistryFile, ValueBlock->DataOffset);
      RtlCopyMemory(DataBlock, Data, DataSize);
      ValueBlock->DataSize = DataSize;
      ValueBlock->DataType = Type;
      CmiReleaseBlock(RegistryFile, DataBlock);
    }
  else
    {
      /*  Destroy current data block and allocate a new one  */
      DataBlock = CmiGetBlock(RegistryFile, ValueBlock->DataOffset);
      Status = CmiAllocateBlock(RegistryFile,
                                &NewDataBlock,
                                DataSize);
      RtlCopyMemory(NewDataBlock, Data, DataSize);
      ValueBlock->DataOffset = CmiGetBlockOffset(RegistryFile, DataBlock);
      ValueBlock->DataSize = DataSize;
      ValueBlock->DataType = Type;
      CmiReleaseBlock(RegistryFile, NewDataBlock);
      CmiDestroyBlock(RegistryFile, DataBlock);
    }

  return  Status;
}

static NTSTATUS
CmiDestroyValueBlock(PREGISTRY_FILE  RegistryFile,
                     PVALUE_BLOCK  ValueBlock)
{
  NTSTATUS  Status;

  Status = CmiDestroyBlock(RegistryFile, 
                           CmiGetBlock(RegistryFile,
                                       ValueBlock->DataOffset));
  if (!NT_SUCCESS(Status))
    {
      return  Status;
    }
  return  CmiDestroyBlock(RegistryFile, ValueBlock);
}

static NTSTATUS
CmiAllocateBlock(PREGISTRY_FILE  RegistryFile,
                 PVOID  *Block,
                 ULONG  BlockSize)
{
  NTSTATUS  Status;
  PVOID  NewBlock;

  Status = STATUS_SUCCESS;

  /*  Handle volatile files first  */
  if (RegistryFile->Filename == NULL)
    {
      NewBlock = ExAllocatePool(NonPagedPool, BlockSize);
      if (NewBlock == NULL)
        {
          Status = STATUS_INSUFFICIENT_RESOURCES;
        }
      else
        {
          RtlZeroMemory(NewBlock, BlockSize);
          CmiLockBlock(RegistryFile, NewBlock);
          *Block = NewBlock;
        }
    }
  else
    {
      Status = STATUS_NOT_IMPLEMENTED;
    }

  return  Status;
}

static NTSTATUS
CmiDestroyBlock(PREGISTRY_FILE  RegistryFile,
                PVOID  Block)
{
  NTSTATUS  Status;

  Status = STATUS_SUCCESS;

  if (RegistryFile->Filename == NULL)
    {
      CmiReleaseBlock(RegistryFile, Block);
      ExFreePool(Block);
    }
  else
    {  
      Status = STATUS_NOT_IMPLEMENTED;
    }

  return  Status;
}

static PVOID
CmiGetBlock(PREGISTRY_FILE  RegistryFile,
            BLOCK_OFFSET  BlockOffset)
{
  PVOID  Block;
  DWORD CurBlock;
  NTSTATUS Status;
  if( BlockOffset == 0 || BlockOffset == -1) return NULL;

  Block = NULL;
  if (RegistryFile->Filename == NULL)
    {
      CmiLockBlock(RegistryFile, (PVOID) BlockOffset);

      Block = (PVOID) BlockOffset;
    }
  else
    {
	PHEAP_BLOCK * tmpBlockList;
	HEAP_BLOCK tmpHeap;
	LARGE_INTEGER fileOffset;
	// search in the heap blocks currently in memory
	for (CurBlock =0; CurBlock  < RegistryFile->BlockListSize ; CurBlock ++)
      {
	  if (  RegistryFile->BlockList[CurBlock ]->BlockOffset <= BlockOffset 
	      && (RegistryFile->BlockList[CurBlock ]->BlockOffset
                   +RegistryFile->BlockList[CurBlock ]->BlockSize > BlockOffset ))
	    return ((char *)RegistryFile->BlockList[CurBlock ]
			+(BlockOffset - RegistryFile->BlockList[CurBlock ]->BlockOffset));
      }
	/* not in memory : read from file */
        /* increase size of list of blocks */
	tmpBlockList=ExAllocatePool(NonPagedPool,
				   sizeof(PHEAP_BLOCK *)*(CurBlock +1));
	if (tmpBlockList == NULL)
	  {
	     KeBugCheck(0);
	     return(FALSE);
	  }
	if(RegistryFile->BlockListSize > 0)
        {
          memcpy(tmpBlockList,RegistryFile->BlockList,
	       sizeof(PHEAP_BLOCK *)*(RegistryFile->BlockListSize ));
	  ExFreePool(RegistryFile->BlockList);
        }
	RegistryFile->BlockList = tmpBlockList;
        /* try to find block at 4K limit under blockOffset */
	fileOffset.u.LowPart = (BlockOffset & 0xfffff000)+REG_BLOCK_SIZE;
	fileOffset.u.HighPart = 0;
        Status = ZwReadFile(RegistryFile->FileHandle, 
                      0, 0, 0, 0, 
                      &tmpHeap, 
                      sizeof(HEAP_BLOCK), 
                      &fileOffset, 0);
        /* if it's not a block, try page 4k before ... */
        /* FIXME : better is to start from previous block in memory */
	while (tmpHeap.BlockId != 0x6e696268 && fileOffset.u.LowPart  >= REG_BLOCK_SIZE)
	{
	   fileOffset.u.LowPart  -= REG_BLOCK_SIZE;
         Status = ZwReadFile(RegistryFile->FileHandle, 
                      0, 0, 0, 0, 
                      &tmpHeap, 
                      sizeof(HEAP_BLOCK), 
                      &fileOffset, 0);
	}
	if (tmpHeap.BlockId != 0x6e696268 )
		return NULL;
        RegistryFile->BlockListSize ++;
	RegistryFile->BlockList [CurBlock]
	   = ExAllocatePool(NonPagedPool,tmpHeap.BlockSize);
CHECKPOINT;
      Status = ZwReadFile(RegistryFile->FileHandle, 
                      0, 0, 0, 0, 
                      RegistryFile->BlockList[CurBlock ],
                      tmpHeap.BlockSize,
                      &fileOffset, 0);
DPRINT(" read %d block file %x octets at %x, Status=%x\n",CurBlock,tmpHeap.BlockSize,fileOffset.u.LowPart,Status);
      Block = ((char *)RegistryFile->BlockList[CurBlock]
			+(BlockOffset - RegistryFile->BlockList[CurBlock]->BlockOffset));
DPRINT(" hbin at %x, block at %x\n",RegistryFile->BlockList[CurBlock],Block);
    }

  return  Block;
}

static BLOCK_OFFSET
CmiGetBlockOffset(PREGISTRY_FILE  RegistryFile,
                  PVOID  Block)
{
  BLOCK_OFFSET  BlockOffset;

  if (RegistryFile->Filename == NULL)
    {
      BlockOffset = (BLOCK_OFFSET) Block;
    }
  else
    {
      UNIMPLEMENTED;
    }

  return BlockOffset;
}

static VOID 
CmiLockBlock(PREGISTRY_FILE  RegistryFile,
             PVOID  Block)
{
  if (RegistryFile->Filename != NULL)
    {
      /* FIXME : implement */
    }
}

static VOID 
CmiReleaseBlock(PREGISTRY_FILE  RegistryFile,
               PVOID  Block)
{
  if (RegistryFile->Filename != NULL)
    {
      /* FIXME : implement */
    }
}


/* EOF */
