/*
 * Usage: rsym input-file output-file
 *
 * There are two sources of information: the .stab/.stabstr
 * sections of the executable and the COFF symbol table. Most
 * of the information is in the .stab/.stabstr sections.
 * However, most of our asm files don't contain .stab directives,
 * so routines implemented in assembler won't show up in the
 * .stab section. They are present in the COFF symbol table.
 * So, we mostly use the .stab/.stabstr sections, but we augment
 * the info there with info from the COFF symbol table when
 * possible.
 *
 * This is a tool and is compiled using the host compiler,
 * i.e. on Linux gcc and not mingw-gcc (cross-compiler).
 * Therefore we can't include SDK headers and we have to
 * duplicate some definitions here.
 * Also note that the internal functions are "old C-style",
 * returning an int, where a return of 0 means success and
 * non-zero is failure.
 */

#include "../../dll/win32/dbghelp/compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <wchar.h>

#include "rsym.h"

#define MAX_PATH 260
#define MAX_SYM_NAME 2000

struct StringEntry
{
    struct StringEntry *Next;
    ULONG Offset;
    char *String;
};

struct StringHashTable
{
    ULONG TableSize;
    struct StringEntry **Table;
};

/* This is the famous DJB hash */
static unsigned int
ComputeDJBHash(const char *name)
{
    unsigned int val = 5381;
    int i = 0;

    for (i = 0; name[i]; i++)
    {
        val = (33 * val) + name[i];
    }

    return val;
}

static void
AddStringToHash(struct StringHashTable *StringTable,
                unsigned int hash,
                ULONG Offset,
                char *StringPtr)
{
    struct StringEntry *entry = calloc(1, sizeof(struct StringEntry));
    entry->Offset = Offset;
    entry->String = StringPtr;
    entry->Next = StringTable->Table[hash];
    StringTable->Table[hash] = entry;
}

static void
StringHashTableInit(struct StringHashTable *StringTable,
                    ULONG StringsLength,
                    char *StringsBase)
{
    char *Start = StringsBase;
    char *End = StringsBase + StringsLength;
    StringTable->TableSize = 1024;
    StringTable->Table = calloc(1024, sizeof(struct StringEntry *));
    while (Start < End)
    {
        AddStringToHash(StringTable,
                        ComputeDJBHash(Start) % StringTable->TableSize,
                        Start - StringsBase,
                        Start);
        Start += strlen(Start) + 1;
    }
}

static void
StringHashTableFree(struct StringHashTable *StringTable)
{
    int i;
    struct StringEntry *entry;
    for (i = 0; i < StringTable->TableSize; i++)
    {
        while ((entry = StringTable->Table[i]))
        {
            entry = entry->Next;
            free(StringTable->Table[i]);
            StringTable->Table[i] = entry;
        }
    }
    free(StringTable->Table);
}

static int
CompareSymEntry(const PROSSYM_ENTRY SymEntry1, const PROSSYM_ENTRY SymEntry2)
{
    if (SymEntry1->Address < SymEntry2->Address)
    {
        return -1;
    }

    if (SymEntry2->Address < SymEntry1->Address)
    {
        return +1;
    }

    if (SymEntry2->SourceLine == 0)
    {
        return -1;
    }

    if (SymEntry1->SourceLine == 0)
    {
        return +1;
    }

    return 0;
}

static int
GetStabInfo(void *FileData, PIMAGE_FILE_HEADER PEFileHeader,
            PIMAGE_SECTION_HEADER PESectionHeaders,
            ULONG *StabSymbolsLength, void **StabSymbolsBase,
            ULONG *StabStringsLength, void **StabStringsBase)
{
    ULONG Idx;

    /* Load .stab and .stabstr sections if available */
    *StabSymbolsBase = NULL;
    *StabSymbolsLength = 0;
    *StabStringsBase = NULL;
    *StabStringsLength = 0;

    for (Idx = 0; Idx < PEFileHeader->NumberOfSections; Idx++)
    {
        /* printf("section: '%.08s'\n", PESectionHeaders[Idx].Name); */
        if ((strncmp((char *) PESectionHeaders[Idx].Name, ".stab", 5) == 0)
            && (PESectionHeaders[Idx].Name[5] == 0))
        {
            /* printf(".stab section found. Size %d\n", PESectionHeaders[Idx].SizeOfRawData); */

            *StabSymbolsLength = PESectionHeaders[Idx].SizeOfRawData;
            *StabSymbolsBase = (void *)((char *) FileData + PESectionHeaders[Idx].PointerToRawData);
        }

        if (strncmp((char *) PESectionHeaders[Idx].Name, ".stabstr", 8) == 0)
        {
            /* printf(".stabstr section found. Size %d\n", PESectionHeaders[Idx].SizeOfRawData); */

            *StabStringsLength = PESectionHeaders[Idx].SizeOfRawData;
            *StabStringsBase = (void *)((char *) FileData + PESectionHeaders[Idx].PointerToRawData);
        }
    }

    return 0;
}

static int
GetCoffInfo(void *FileData, PIMAGE_FILE_HEADER PEFileHeader,
            PIMAGE_SECTION_HEADER PESectionHeaders,
            ULONG *CoffSymbolsLength, void **CoffSymbolsBase,
            ULONG *CoffStringsLength, void **CoffStringsBase)
{

    if (PEFileHeader->PointerToSymbolTable == 0 || PEFileHeader->NumberOfSymbols == 0)
    {
        /* No COFF symbol table */
        *CoffSymbolsLength = 0;
        *CoffStringsLength = 0;
    }
    else
    {
        *CoffSymbolsLength = PEFileHeader->NumberOfSymbols * sizeof(COFF_SYMENT);
        *CoffSymbolsBase = (void *)((char *) FileData + PEFileHeader->PointerToSymbolTable);
        *CoffStringsLength = *((ULONG *) ((char *) *CoffSymbolsBase + *CoffSymbolsLength));
        *CoffStringsBase = (void *)((char *) *CoffSymbolsBase + *CoffSymbolsLength);
    }

    return 0;
}

static ULONG
FindOrAddString(struct StringHashTable *StringTable,
                char *StringToFind,
                ULONG *StringsLength,
                void *StringsBase)
{
    unsigned int hash = ComputeDJBHash(StringToFind) % StringTable->TableSize;
    struct StringEntry *entry = StringTable->Table[hash];

    while (entry && strcmp(entry->String, StringToFind))
        entry = entry->Next;

    if (entry)
    {
        return entry->Offset;
    }
    else
    {
        char *End = (char *)StringsBase + *StringsLength;

        strcpy(End, StringToFind);
        *StringsLength += strlen(StringToFind) + 1;

        AddStringToHash(StringTable, hash, End - (char *)StringsBase, End);

        return End - (char *)StringsBase;
    }
}

static int
ConvertStabs(ULONG *SymbolsCount, PROSSYM_ENTRY *SymbolsBase,
             ULONG *StringsLength, void *StringsBase,
             ULONG StabSymbolsLength, void *StabSymbolsBase,
             ULONG StabStringsLength, void *StabStringsBase,
             ULONG_PTR ImageBase, PIMAGE_FILE_HEADER PEFileHeader,
             PIMAGE_SECTION_HEADER PESectionHeaders)
{
    PSTAB_ENTRY StabEntry;
    ULONG Count, i;
    ULONG_PTR Address, LastFunctionAddress;
    int First = 1;
    char *Name;
    ULONG NameLen;
    char FuncName[256];
    PROSSYM_ENTRY Current;
    struct StringHashTable StringHash;

    StabEntry = StabSymbolsBase;
    Count = StabSymbolsLength / sizeof(STAB_ENTRY);
    *SymbolsCount = 0;

    if (Count == 0)
    {
        /* No symbol info */
        *SymbolsBase = NULL;
        return 0;
    }

    *SymbolsBase = malloc(Count * sizeof(ROSSYM_ENTRY));
    if (*SymbolsBase == NULL)
    {
        fprintf(stderr, "Failed to allocate memory for converted .stab symbols\n");
        return 1;
    }
    Current = *SymbolsBase;
    memset(Current, 0, sizeof(*Current));

    StringHashTableInit(&StringHash, *StringsLength, (char *)StringsBase);

    LastFunctionAddress = 0;
    for (i = 0; i < Count; i++)
    {
        if (LastFunctionAddress == 0)
        {
            Address = StabEntry[i].n_value - ImageBase;
        }
        else
        {
            Address = LastFunctionAddress + StabEntry[i].n_value;
        }
        switch (StabEntry[i].n_type)
        {
            case N_SO:
            case N_SOL:
            case N_BINCL:
                Name = (char *) StabStringsBase + StabEntry[i].n_strx;
                if (StabStringsLength < StabEntry[i].n_strx
                    || *Name == '\0' || Name[strlen(Name) - 1] == '/'
                    || Name[strlen(Name) - 1] == '\\'
                    || StabEntry[i].n_value < ImageBase)
                {
                    continue;
                }
                if (First || Address != Current->Address)
                {
                    if (!First)
                    {
                        memset(++Current, 0, sizeof(*Current));
                        Current->FunctionOffset = Current[-1].FunctionOffset;
                    }
                    else
                        First = 0;
                    Current->Address = Address;
                }
                Current->FileOffset = FindOrAddString(&StringHash,
                                                      (char *)StabStringsBase + StabEntry[i].n_strx,
                                                      StringsLength,
                                                      StringsBase);
                break;
            case N_FUN:
                if (StabEntry[i].n_desc == 0 || StabEntry[i].n_value < ImageBase)
                {
                    LastFunctionAddress = 0; /* line # 0 = end of function */
                    continue;
                }
                if (First || Address != Current->Address)
                {
                    if (!First)
                        memset(++Current, 0, sizeof(*Current));
                    else
                        First = 0;
                    Current->Address = Address;
                    Current->FileOffset = Current[-1].FileOffset;
                }
                Name = (char *)StabStringsBase + StabEntry[i].n_strx;
                NameLen = strcspn(Name, ":");
                if (sizeof(FuncName) <= NameLen)
                {
                    free(*SymbolsBase);
                    fprintf(stderr, "Function name too long\n");
                    return 1;
                }
                memcpy(FuncName, Name, NameLen);
                FuncName[NameLen] = '\0';
                Current->FunctionOffset = FindOrAddString(&StringHash,
                                                          FuncName,
                                                          StringsLength,
                                                          StringsBase);
                Current->SourceLine = 0;
                LastFunctionAddress = Address;
                break;
            case N_SLINE:
                if (First || Address != Current->Address)
                {
                    if (!First)
                    {
                        memset(++Current, 0, sizeof(*Current));
                        Current->FileOffset = Current[-1].FileOffset;
                        Current->FunctionOffset = Current[-1].FunctionOffset;
                    }
                    else
                        First = 0;
                    Current->Address = Address;
                }
                Current->SourceLine = StabEntry[i].n_desc;
                break;
            default:
                continue;
        }
    }
    *SymbolsCount = (Current - *SymbolsBase + 1);

    qsort(*SymbolsBase, *SymbolsCount, sizeof(ROSSYM_ENTRY), (int (*)(const void *, const void *)) CompareSymEntry);

    StringHashTableFree(&StringHash);

    return 0;
}

static int
ConvertCoffs(ULONG *SymbolsCount, PROSSYM_ENTRY *SymbolsBase,
             ULONG *StringsLength, void *StringsBase,
             ULONG CoffSymbolsLength, void *CoffSymbolsBase,
             ULONG CoffStringsLength, void *CoffStringsBase,
             ULONG_PTR ImageBase, PIMAGE_FILE_HEADER PEFileHeader,
             PIMAGE_SECTION_HEADER PESectionHeaders)
{
    ULONG Count, i;
    PCOFF_SYMENT CoffEntry;
    char FuncName[256], FileName[1024];
    char *p;
    PROSSYM_ENTRY Current;
    struct StringHashTable StringHash;

    CoffEntry = (PCOFF_SYMENT) CoffSymbolsBase;
    Count = CoffSymbolsLength / sizeof(COFF_SYMENT);

    *SymbolsBase = malloc(Count * sizeof(ROSSYM_ENTRY));
    if (*SymbolsBase == NULL)
    {
        fprintf(stderr, "Unable to allocate memory for converted COFF symbols\n");
        return 1;
    }
    *SymbolsCount = 0;
    Current = *SymbolsBase;

    StringHashTableInit(&StringHash, *StringsLength, (char*)StringsBase);

    for (i = 0; i < Count; i++)
    {
        if (ISFCN(CoffEntry[i].e_type) || C_EXT == CoffEntry[i].e_sclass)
        {
            Current->Address = CoffEntry[i].e_value;
            if (CoffEntry[i].e_scnum > 0)
            {
                if (PEFileHeader->NumberOfSections < CoffEntry[i].e_scnum)
                {
                    free(*SymbolsBase);
                    fprintf(stderr,
                            "Invalid section number %d in COFF symbols (only %d sections present)\n",
                            CoffEntry[i].e_scnum,
                            PEFileHeader->NumberOfSections);
                    return 1;
                }
                Current->Address += PESectionHeaders[CoffEntry[i].e_scnum - 1].VirtualAddress;
            }
            Current->FileOffset = 0;
            if (CoffEntry[i].e.e.e_zeroes == 0)
            {
                if (sizeof(FuncName) <= strlen((char *) CoffStringsBase + CoffEntry[i].e.e.e_offset))
                {
                    free(*SymbolsBase);
                    fprintf(stderr, "Function name too long\n");
                    StringHashTableFree(&StringHash);
                    return 1;
                }
                strcpy(FuncName, (char *) CoffStringsBase + CoffEntry[i].e.e.e_offset);
            }
            else
            {
                memcpy(FuncName, CoffEntry[i].e.e_name, E_SYMNMLEN);
                FuncName[E_SYMNMLEN] = '\0';
            }

            /* Name demangling: stdcall */
            p = strrchr(FuncName, '@');
            if (p != NULL)
            {
                *p = '\0';
            }
            p = ('_' == FuncName[0] || '@' == FuncName[0] ? FuncName + 1 : FuncName);
            Current->FunctionOffset = FindOrAddString(&StringHash,
                                                      p,
                                                      StringsLength,
                                                      StringsBase);
            Current->SourceLine = 0;
            memset(++Current, 0, sizeof(*Current));
        }

        i += CoffEntry[i].e_numaux;
    }

    *SymbolsCount = (Current - *SymbolsBase + 1);
    qsort(*SymbolsBase, *SymbolsCount, sizeof(ROSSYM_ENTRY), (int (*)(const void *, const void *)) CompareSymEntry);

    StringHashTableFree(&StringHash);

    return 0;
}

struct DbgHelpLineEntry {
  ULONG vma;
  ULONG fileId;
  ULONG functionId;
  ULONG line;
};

struct DbgHelpStringTab {
  ULONG Length;
  ULONG Bytes;
  char ***Table;
  ULONG LineEntries, CurLineEntries;
  struct DbgHelpLineEntry *LineEntryData;
  void *process;
  DWORD module_base;
  char *PathChop;
  char *SourcePath;
  struct DbgHelpLineEntry *lastLineEntry;
};

static struct DbgHelpLineEntry*
DbgHelpAddLineEntry(struct DbgHelpStringTab *tab)
{
    if (tab->CurLineEntries == tab->LineEntries)
    {
        struct DbgHelpLineEntry *newEntries = realloc(tab->LineEntryData,
                                                      tab->LineEntries * 2 * sizeof(struct DbgHelpLineEntry));

        if (!newEntries)
            return 0;

        tab->LineEntryData = newEntries;

        memset(tab->LineEntryData + tab->LineEntries, 0, sizeof(struct DbgHelpLineEntry) * tab->LineEntries);
        tab->LineEntries *= 2;
    }

    return &tab->LineEntryData[tab->CurLineEntries++];
}

static int
DbgHelpAddStringToTable(struct DbgHelpStringTab *tab, char *name)
{
    unsigned int bucket = ComputeDJBHash(name) % tab->Length;
    char **tabEnt = tab->Table[bucket];
    int i;
    char **newBucket;

    if (tabEnt)
    {
        for (i = 0; tabEnt[i] && strcmp(tabEnt[i], name); i++);
        if (tabEnt[i])
        {
            free(name);
            return (i << 10) | bucket;
        }
    }
    else
        i = 0;

    /* At this point, we need to insert */
    tab->Bytes += strlen(name) + 1;

    newBucket = realloc(tab->Table[bucket], (i+2) * sizeof(char *));

    if (!newBucket)
    {
        fprintf(stderr, "realloc failed!\n");
        return -1;
    }

    tab->Table[bucket] = newBucket;
    tab->Table[bucket][i+1] = 0;
    tab->Table[bucket][i] = name;
    return (i << 10) | bucket;
}

const char*
DbgHelpGetString(struct DbgHelpStringTab *tab, int id)
{
    int i = id >> 10;
    int bucket = id & 0x3ff;
    return tab->Table[bucket][i];
}

/* Remove a prefix of PathChop if it exists and return a copy of the tail. */
static char *
StrDupShortenPath(char *PathChop, char *FilePath)
{
    int pclen = strlen(PathChop);
    if (!strncmp(FilePath, PathChop, pclen))
    {
        return strdup(FilePath+pclen);
    }
    else
    {
        return strdup(FilePath);
    }
}

static BOOL
DbgHelpAddLineNumber(PSRCCODEINFO LineInfo, void *UserContext)
{
    struct DbgHelpStringTab *tab = (struct DbgHelpStringTab *)UserContext;
    DWORD64 disp;
    int fileId, functionId;
    PSYMBOL_INFO pSymbol = malloc(FIELD_OFFSET(SYMBOL_INFO, Name[MAX_SYM_NAME]));
    if (!pSymbol) return FALSE;
    memset(pSymbol, 0, FIELD_OFFSET(SYMBOL_INFO, Name[MAX_SYM_NAME]));

    /* If any file can be opened by relative path up to a certain level, then
       record that path. */
    if (!tab->PathChop)
    {
        int i, endLen;
        char *end = strrchr(LineInfo->FileName, '/');

        if (!end)
            end = strrchr(LineInfo->FileName, '\\');

        if (end)
        {
            for (i = (end - LineInfo->FileName) - 1; i >= 0; i--)
            {
                if (LineInfo->FileName[i] == '/' || LineInfo->FileName[i] == '\\')
                {
                    char *synthname = malloc(strlen(tab->SourcePath) +
                                             strlen(LineInfo->FileName + i + 1)
                                             + 2);
                    strcpy(synthname, tab->SourcePath);
                    strcat(synthname, "/");
                    strcat(synthname, LineInfo->FileName + i + 1);
                    FILE *f = fopen(synthname, "r");
                    free(synthname);
                    if (f)
                    {
                        fclose(f);
                        break;
                    }
                }
            }

            i++; /* Be in the string or past the next slash */
            tab->PathChop = malloc(i + 1);
            memcpy(tab->PathChop, LineInfo->FileName, i);
            tab->PathChop[i] = 0;
        }
    }

    fileId = DbgHelpAddStringToTable(tab,
                                     StrDupShortenPath(tab->PathChop,
                                                       LineInfo->FileName));

    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    if (!SymFromAddr(tab->process, LineInfo->Address, &disp, pSymbol))
    {
        //fprintf(stderr, "SymFromAddr failed.\n");
        free(pSymbol);
        return FALSE;
    }

    functionId = DbgHelpAddStringToTable(tab, strdup(pSymbol->Name));

    if (LineInfo->Address == 0)
        fprintf(stderr, "Address is 0.\n");

    tab->lastLineEntry = DbgHelpAddLineEntry(tab);
    tab->lastLineEntry->vma = LineInfo->Address - LineInfo->ModBase;
    tab->lastLineEntry->functionId = functionId;
    tab->lastLineEntry->fileId = fileId;
    tab->lastLineEntry->line = LineInfo->LineNumber;

    free(pSymbol);
    return TRUE;
}

static int
ConvertDbgHelp(void *process, DWORD module_base, char *SourcePath,
               ULONG *SymbolsCount, PROSSYM_ENTRY *SymbolsBase,
               ULONG *StringsLength, void **StringsBase)
{
    char *strings, *strings_copy;
    int i, j, bucket, entry;
    PROSSYM_ENTRY rossym;
    struct DbgHelpStringTab strtab = { 0 };

    strtab.process = process;
    strtab.module_base = module_base;
    strtab.Bytes = 1;
    strtab.Length = 1024;
    strtab.Table = calloc(1024, sizeof(const char **));
    strtab.Table[0] = calloc(2, sizeof(const char *));
    strtab.Table[0][0] = strdup(""); // The zero string
    strtab.CurLineEntries = 0;
    strtab.LineEntries = 16384;
    strtab.LineEntryData = calloc(strtab.LineEntries, sizeof(struct DbgHelpLineEntry));
    strtab.PathChop = NULL;
    strtab.SourcePath = SourcePath ? SourcePath : "";

    SymEnumLines(process, module_base, NULL, NULL, DbgHelpAddLineNumber, &strtab);

    /* Transcribe necessary strings */
    *StringsLength = strtab.Bytes;
    strings = strings_copy = ((char *)(*StringsBase = malloc(strtab.Bytes)));

    /* Copy in strings */
    for (i = 0; i < strtab.Length; i++)
    {
        for (j = 0; strtab.Table[i] && strtab.Table[i][j]; j++)
        {
            /* Each entry is replaced by its corresponding entry in our string
               section. We can substract the strings origin to get an offset. */
            char *toFree = strtab.Table[i][j];
            strtab.Table[i][j] = strcpy(strings_copy, strtab.Table[i][j]);
            free(toFree);
            strings_copy += strlen(strings_copy) + 1;
        }
    }

    assert(strings_copy == strings + strtab.Bytes);

    *SymbolsBase = calloc(strtab.CurLineEntries, sizeof(ROSSYM_ENTRY));
    *SymbolsCount = strtab.CurLineEntries;

    /* Copy symbols into rossym entries */
    for (i = 0; i < strtab.CurLineEntries; i++)
    {
        rossym = &(*SymbolsBase)[i];
        rossym->Address = strtab.LineEntryData[i].vma;
        bucket = strtab.LineEntryData[i].fileId & 0x3ff;
        entry = strtab.LineEntryData[i].fileId >> 10;
        rossym->FileOffset = strtab.Table[bucket][entry] - strings;
        bucket = strtab.LineEntryData[i].functionId & 0x3ff;
        entry = strtab.LineEntryData[i].functionId >> 10;
        rossym->FunctionOffset = strtab.Table[bucket][entry] - strings;
        rossym->SourceLine = strtab.LineEntryData[i].line;
    }

    /* Free stringtab */
    for (i = 0; i < strtab.Length; i++)
    {
        free(strtab.Table[i]);
    }

    free(strtab.LineEntryData);
    free(strtab.PathChop);

    qsort(*SymbolsBase, *SymbolsCount, sizeof(ROSSYM_ENTRY), (int (*)(const void *, const void *))CompareSymEntry);

    return 0;
}

static int
MergeStabsAndCoffs(ULONG *MergedSymbolCount, PROSSYM_ENTRY *MergedSymbols,
                   ULONG StabSymbolsCount, PROSSYM_ENTRY StabSymbols,
                   ULONG CoffSymbolsCount, PROSSYM_ENTRY CoffSymbols)
{
    ULONG StabIndex, j;
    ULONG CoffIndex;
    ULONG_PTR StabFunctionStartAddress;
    ULONG StabFunctionStringOffset, NewStabFunctionStringOffset;

    *MergedSymbolCount = 0;
    if (StabSymbolsCount == 0)
    {
        *MergedSymbols = NULL;
        return 0;
    }
    *MergedSymbols = malloc((StabSymbolsCount + CoffSymbolsCount) * sizeof(ROSSYM_ENTRY));
    if (*MergedSymbols == NULL)
    {
        fprintf(stderr, "Unable to allocate memory for merged symbols\n");
        return 1;
    }

    StabFunctionStartAddress = 0;
    StabFunctionStringOffset = 0;
    CoffIndex = 0;
    for (StabIndex = 0; StabIndex < StabSymbolsCount; StabIndex++)
    {
        (*MergedSymbols)[*MergedSymbolCount] = StabSymbols[StabIndex];
        for (j = StabIndex + 1;
             j < StabSymbolsCount && StabSymbols[j].Address == StabSymbols[StabIndex].Address;
             j++)
        {
            if (StabSymbols[j].FileOffset != 0 && (*MergedSymbols)[*MergedSymbolCount].FileOffset == 0)
            {
                (*MergedSymbols)[*MergedSymbolCount].FileOffset = StabSymbols[j].FileOffset;
            }
            if (StabSymbols[j].FunctionOffset != 0 && (*MergedSymbols)[*MergedSymbolCount].FunctionOffset == 0)
            {
                (*MergedSymbols)[*MergedSymbolCount].FunctionOffset = StabSymbols[j].FunctionOffset;
            }
            if (StabSymbols[j].SourceLine != 0 && (*MergedSymbols)[*MergedSymbolCount].SourceLine == 0)
            {
                (*MergedSymbols)[*MergedSymbolCount].SourceLine = StabSymbols[j].SourceLine;
            }
        }
        StabIndex = j - 1;

        while (CoffIndex < CoffSymbolsCount &&
               CoffSymbols[CoffIndex + 1].Address <= (*MergedSymbols)[*MergedSymbolCount].Address)
        {
            CoffIndex++;
        }
        NewStabFunctionStringOffset = (*MergedSymbols)[*MergedSymbolCount].FunctionOffset;
        if (CoffSymbolsCount > 0 &&
            CoffSymbols[CoffIndex].Address < (*MergedSymbols)[*MergedSymbolCount].Address &&
            StabFunctionStartAddress < CoffSymbols[CoffIndex].Address &&
            CoffSymbols[CoffIndex].FunctionOffset != 0)
        {
            (*MergedSymbols)[*MergedSymbolCount].FunctionOffset = CoffSymbols[CoffIndex].FunctionOffset;
            CoffSymbols[CoffIndex].FileOffset = CoffSymbols[CoffIndex].FunctionOffset = 0;
        }
        if (StabFunctionStringOffset != NewStabFunctionStringOffset)
        {
            StabFunctionStartAddress = (*MergedSymbols)[*MergedSymbolCount].Address;
        }
        StabFunctionStringOffset = NewStabFunctionStringOffset;
        (*MergedSymbolCount)++;
    }
    /* Handle functions that have no analog in the upstream data */
    for (CoffIndex = 0; CoffIndex < CoffSymbolsCount; CoffIndex++)
    {
        if (CoffSymbols[CoffIndex].Address &&
            CoffSymbols[CoffIndex].FunctionOffset)
        {
            (*MergedSymbols)[*MergedSymbolCount] = CoffSymbols[CoffIndex];
            (*MergedSymbolCount)++;
        }
    }

    qsort(*MergedSymbols, *MergedSymbolCount, sizeof(ROSSYM_ENTRY), (int (*)(const void *, const void *)) CompareSymEntry);

    return 0;
}

static PIMAGE_SECTION_HEADER
FindSectionForRVA(DWORD RVA, unsigned NumberOfSections, PIMAGE_SECTION_HEADER SectionHeaders)
{
    unsigned Section;

    for (Section = 0; Section < NumberOfSections; Section++)
    {
        if (SectionHeaders[Section].VirtualAddress <= RVA &&
            RVA < SectionHeaders[Section].VirtualAddress + SectionHeaders[Section].Misc.VirtualSize)
        {
            return SectionHeaders + Section;
        }
    }

    return NULL;
}

static int
ProcessRelocations(ULONG *ProcessedRelocsLength, void **ProcessedRelocs,
                   void *RawData, PIMAGE_OPTIONAL_HEADER OptHeader,
                   unsigned NumberOfSections, PIMAGE_SECTION_HEADER SectionHeaders)
{
    PIMAGE_SECTION_HEADER RelocSectionHeader, TargetSectionHeader;
    PIMAGE_BASE_RELOCATION BaseReloc, End, AcceptedRelocs;
    int Found;

    if (OptHeader->NumberOfRvaAndSizes < IMAGE_DIRECTORY_ENTRY_BASERELOC ||
        OptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0)
    {
        /* No relocation entries */
        *ProcessedRelocsLength = 0;
        *ProcessedRelocs = NULL;
        return 0;
    }

    RelocSectionHeader = FindSectionForRVA(OptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress,
                                           NumberOfSections, SectionHeaders);
    if (RelocSectionHeader == NULL)
    {
        fprintf(stderr, "Can't find section header for relocation data\n");
        return 1;
    }

    *ProcessedRelocs = malloc(RelocSectionHeader->SizeOfRawData);
    if (*ProcessedRelocs == NULL)
    {
        fprintf(stderr,
                "Failed to allocate %u bytes for relocations\n",
                (unsigned int)RelocSectionHeader->SizeOfRawData);
        return 1;
    }
    *ProcessedRelocsLength = 0;

    BaseReloc = (PIMAGE_BASE_RELOCATION) ((char *) RawData +
                                          RelocSectionHeader->PointerToRawData +
                                          (OptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress -
                                           RelocSectionHeader->VirtualAddress));
    End = (PIMAGE_BASE_RELOCATION) ((char *) BaseReloc +
                                    OptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);

    while (BaseReloc < End && BaseReloc->SizeOfBlock > 0)
    {
        TargetSectionHeader = FindSectionForRVA(BaseReloc->VirtualAddress,
                                                NumberOfSections,
                                                SectionHeaders);
        if (TargetSectionHeader != NULL)
        {
            AcceptedRelocs = *ProcessedRelocs;
            Found = 0;
            while (AcceptedRelocs < (PIMAGE_BASE_RELOCATION) ((char *) *ProcessedRelocs +
                                                              *ProcessedRelocsLength)
                   && !Found)
            {
                Found = BaseReloc->SizeOfBlock == AcceptedRelocs->SizeOfBlock &&
                                                  memcmp(BaseReloc, AcceptedRelocs, AcceptedRelocs->SizeOfBlock) == 0;
                AcceptedRelocs = (PIMAGE_BASE_RELOCATION) ((char *) AcceptedRelocs +
                                                           AcceptedRelocs->SizeOfBlock);
            }
            if (!Found)
            {
                memcpy((char *) *ProcessedRelocs + *ProcessedRelocsLength,
                       BaseReloc,
                       BaseReloc->SizeOfBlock);
                *ProcessedRelocsLength += BaseReloc->SizeOfBlock;
            }
        }
        BaseReloc = (PIMAGE_BASE_RELOCATION)((char *) BaseReloc + BaseReloc->SizeOfBlock);
    }

    return 0;
}

static const BYTE*
GetSectionName(void *StringsBase, const BYTE *SectionTitle)
{
    if (SectionTitle[0] == '/')
    {
        int offset = atoi((char*)SectionTitle+1);
        return ((BYTE *)StringsBase) + offset;
    }
    else
        return SectionTitle;
}

static int
CreateOutputFile(FILE *OutFile, void *InData,
                 PIMAGE_DOS_HEADER InDosHeader, PIMAGE_FILE_HEADER InFileHeader,
                 PIMAGE_OPTIONAL_HEADER InOptHeader, PIMAGE_SECTION_HEADER InSectionHeaders,
                 ULONG RosSymLength, void *RosSymSection)
{
    ULONG StartOfRawData;
    unsigned Section;
    void *OutHeader, *ProcessedRelocs, *PaddedRosSym, *Data;
    unsigned char *PaddedStringTable;
    PIMAGE_DOS_HEADER OutDosHeader;
    PIMAGE_FILE_HEADER OutFileHeader;
    PIMAGE_OPTIONAL_HEADER OutOptHeader;
    PIMAGE_SECTION_HEADER OutSectionHeaders, CurrentSectionHeader;
    DWORD CheckSum;
    ULONG Length, i;
    ULONG ProcessedRelocsLength;
    ULONG RosSymOffset, RosSymFileLength;
    ULONG PaddedStringTableLength;
    int InRelocSectionIndex;
    PIMAGE_SECTION_HEADER OutRelocSection;
    /* Each coff symbol is 18 bytes and the string table follows */
    char *StringTable = (char *)InData +
        InFileHeader->PointerToSymbolTable + 18 * InFileHeader->NumberOfSymbols;
    ULONG StringTableLength = 0;
    ULONG StringTableLocation;

    StartOfRawData = 0;
    for (Section = 0; Section < InFileHeader->NumberOfSections; Section++)
    {
        const BYTE *SectionName = GetSectionName(StringTable,
                                                 InSectionHeaders[Section].Name);
        if (InSectionHeaders[Section].Name[0] == '/')
        {
            StringTableLength = atoi((const char *)InSectionHeaders[Section].Name + 1) +
                                strlen((const char *)SectionName) + 1;
        }
        if ((StartOfRawData == 0 || InSectionHeaders[Section].PointerToRawData < StartOfRawData)
            && InSectionHeaders[Section].PointerToRawData != 0
            && (strncmp((char *) SectionName, ".stab", 5)) != 0
            && (strncmp((char *) SectionName, ".debug_", 7)) != 0)
        {
            StartOfRawData = InSectionHeaders[Section].PointerToRawData;
        }
    }
    OutHeader = malloc(StartOfRawData);
    if (OutHeader == NULL)
    {
        fprintf(stderr,
                "Failed to allocate %u bytes for output file header\n",
                (unsigned int)StartOfRawData);
        return 1;
    }
    memset(OutHeader, '\0', StartOfRawData);

    OutDosHeader = (PIMAGE_DOS_HEADER) OutHeader;
    memcpy(OutDosHeader, InDosHeader, InDosHeader->e_lfanew + sizeof(ULONG));

    OutFileHeader = (PIMAGE_FILE_HEADER)((char *) OutHeader + OutDosHeader->e_lfanew + sizeof(ULONG));
    memcpy(OutFileHeader, InFileHeader, sizeof(IMAGE_FILE_HEADER));
    OutFileHeader->PointerToSymbolTable = 0;
    OutFileHeader->NumberOfSymbols = 0;
    OutFileHeader->Characteristics &= ~(IMAGE_FILE_LINE_NUMS_STRIPPED | IMAGE_FILE_LOCAL_SYMS_STRIPPED |
                                        IMAGE_FILE_DEBUG_STRIPPED);

    OutOptHeader = (PIMAGE_OPTIONAL_HEADER)(OutFileHeader + 1);
    memcpy(OutOptHeader, InOptHeader, sizeof(IMAGE_OPTIONAL_HEADER));
    OutOptHeader->CheckSum = 0;

    OutSectionHeaders = (PIMAGE_SECTION_HEADER)((char *) OutOptHeader + OutFileHeader->SizeOfOptionalHeader);

    if (ProcessRelocations(&ProcessedRelocsLength,
                           &ProcessedRelocs,
                           InData,
                           InOptHeader,
                           InFileHeader->NumberOfSections,
                           InSectionHeaders))
    {
        return 1;
    }
    if (InOptHeader->NumberOfRvaAndSizes < IMAGE_DIRECTORY_ENTRY_BASERELOC ||
        InOptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0)
    {
        InRelocSectionIndex = -1;
    }
    else
    {
        InRelocSectionIndex = FindSectionForRVA(InOptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress,
                                                InFileHeader->NumberOfSections, InSectionHeaders) - InSectionHeaders;
    }

    OutFileHeader->NumberOfSections = 0;
    CurrentSectionHeader = OutSectionHeaders;
    OutOptHeader->SizeOfImage = 0;
    RosSymOffset = 0;
    OutRelocSection = NULL;

    StringTableLocation = StartOfRawData;

    for (Section = 0; Section < InFileHeader->NumberOfSections; Section++)
    {
        const BYTE *SectionName = GetSectionName(StringTable,
                                                 InSectionHeaders[Section].Name);
        if ((strncmp((char *) SectionName, ".stab", 5) != 0) &&
            (strncmp((char *) SectionName, ".debug_", 7)) != 0)
        {
            *CurrentSectionHeader = InSectionHeaders[Section];
            CurrentSectionHeader->PointerToLinenumbers = 0;
            CurrentSectionHeader->NumberOfLinenumbers = 0;
            if (OutOptHeader->SizeOfImage < CurrentSectionHeader->VirtualAddress +
                                            CurrentSectionHeader->Misc.VirtualSize)
            {
                OutOptHeader->SizeOfImage = ROUND_UP(CurrentSectionHeader->VirtualAddress +
                                                     CurrentSectionHeader->Misc.VirtualSize,
                                                     OutOptHeader->SectionAlignment);
            }
            if (RosSymOffset < CurrentSectionHeader->PointerToRawData + CurrentSectionHeader->SizeOfRawData)
            {
                RosSymOffset = CurrentSectionHeader->PointerToRawData + CurrentSectionHeader->SizeOfRawData;
            }
            if (Section == (ULONG)InRelocSectionIndex)
            {
                OutRelocSection = CurrentSectionHeader;
            }
            StringTableLocation = CurrentSectionHeader->PointerToRawData + CurrentSectionHeader->SizeOfRawData;
            OutFileHeader->NumberOfSections++;
            CurrentSectionHeader++;
        }
    }

    if (OutRelocSection == CurrentSectionHeader - 1)
    {
        OutOptHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = ProcessedRelocsLength;
        if (OutOptHeader->SizeOfImage == OutRelocSection->VirtualAddress +
                                         ROUND_UP(OutRelocSection->Misc.VirtualSize,
                                                  OutOptHeader->SectionAlignment))
        {
            OutOptHeader->SizeOfImage = OutRelocSection->VirtualAddress +
                                        ROUND_UP(ProcessedRelocsLength,
                                                 OutOptHeader->SectionAlignment);
        }
        OutRelocSection->Misc.VirtualSize = ProcessedRelocsLength;
        if (RosSymOffset == OutRelocSection->PointerToRawData +
                            OutRelocSection->SizeOfRawData)
        {
            RosSymOffset = OutRelocSection->PointerToRawData +
                           ROUND_UP(ProcessedRelocsLength,
                                    OutOptHeader->FileAlignment);
        }
        OutRelocSection->SizeOfRawData = ROUND_UP(ProcessedRelocsLength,
                                                  OutOptHeader->FileAlignment);
    }

    if (RosSymLength > 0)
    {
        RosSymFileLength = ROUND_UP(RosSymLength, OutOptHeader->FileAlignment);
        memcpy(CurrentSectionHeader->Name, ".rossym", 8); /* We're lucky: string is exactly 8 bytes long */
        CurrentSectionHeader->Misc.VirtualSize = RosSymLength;
        CurrentSectionHeader->VirtualAddress = OutOptHeader->SizeOfImage;
        CurrentSectionHeader->SizeOfRawData = RosSymFileLength;
        CurrentSectionHeader->PointerToRawData = RosSymOffset;
        CurrentSectionHeader->PointerToRelocations = 0;
        CurrentSectionHeader->PointerToLinenumbers = 0;
        CurrentSectionHeader->NumberOfRelocations = 0;
        CurrentSectionHeader->NumberOfLinenumbers = 0;
        CurrentSectionHeader->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE
                                                | IMAGE_SCN_LNK_REMOVE | IMAGE_SCN_TYPE_NOLOAD;
        OutOptHeader->SizeOfImage = ROUND_UP(CurrentSectionHeader->VirtualAddress + CurrentSectionHeader->Misc.VirtualSize,
                                             OutOptHeader->SectionAlignment);
        OutFileHeader->NumberOfSections++;

        PaddedRosSym = malloc(RosSymFileLength);
        if (PaddedRosSym == NULL)
        {
            fprintf(stderr,
                    "Failed to allocate %u bytes for padded .rossym\n",
                    (unsigned int)RosSymFileLength);
            return 1;
        }
        memcpy(PaddedRosSym, RosSymSection, RosSymLength);
        memset((char *) PaddedRosSym + RosSymLength,
               '\0',
               RosSymFileLength - RosSymLength);

        /* Position the string table after our new section */
        StringTableLocation = RosSymOffset + RosSymFileLength;
    }
    else
    {
        PaddedRosSym = NULL;
    }

    /* Set the string table area in the header if we need it */
    if (StringTableLength)
    {
        OutFileHeader->PointerToSymbolTable = StringTableLocation;
        OutFileHeader->NumberOfSymbols = 0;
    }

    CheckSum = 0;
    for (i = 0; i < StartOfRawData / 2; i++)
    {
        CheckSum += ((unsigned short*) OutHeader)[i];
        CheckSum = 0xffff & (CheckSum + (CheckSum >> 16));
    }
    Length = StartOfRawData;
    for (Section = 0; Section < OutFileHeader->NumberOfSections; Section++)
    {
        DWORD SizeOfRawData;
        if (OutRelocSection == OutSectionHeaders + Section)
        {
            Data = (void *) ProcessedRelocs;
            SizeOfRawData = ProcessedRelocsLength;
        }
        else if (RosSymLength > 0 && Section + 1 == OutFileHeader->NumberOfSections)
        {
            Data = (void *) PaddedRosSym;
            SizeOfRawData = OutSectionHeaders[Section].SizeOfRawData;
        }
        else
        {
            Data = (void *) ((char *) InData + OutSectionHeaders[Section].PointerToRawData);
            SizeOfRawData = OutSectionHeaders[Section].SizeOfRawData;
        }
        for (i = 0; i < SizeOfRawData / 2; i++)
        {
            CheckSum += ((unsigned short*) Data)[i];
            CheckSum = 0xffff & (CheckSum + (CheckSum >> 16));
        }
        Length += OutSectionHeaders[Section].SizeOfRawData;
    }

    if (OutFileHeader->PointerToSymbolTable)
    {
        int PaddingFrom = (OutFileHeader->PointerToSymbolTable + StringTableLength) %
                          OutOptHeader->FileAlignment;
        int PaddingSize = PaddingFrom ? OutOptHeader->FileAlignment - PaddingFrom : 0;

        PaddedStringTableLength = StringTableLength + PaddingSize;
        PaddedStringTable = malloc(PaddedStringTableLength);
        /* COFF string section is preceeded by a length */
        assert(sizeof(StringTableLength) == 4);
        memcpy(PaddedStringTable, &StringTableLength, sizeof(StringTableLength));
        /* We just copy enough of the string table to contain the strings we want
           The string table length technically counts as part of the string table
           space itself. */
        memcpy(PaddedStringTable + 4, StringTable + 4, StringTableLength - 4);
        memset(PaddedStringTable + StringTableLength, 0, PaddingSize);

        assert(OutFileHeader->PointerToSymbolTable % 2 == 0);
        for (i = 0; i < PaddedStringTableLength / 2; i++)
        {
            CheckSum += ((unsigned short*)PaddedStringTable)[i];
            CheckSum = 0xffff & (CheckSum + (CheckSum >> 16));
        }
        Length += PaddedStringTableLength;
    }
    else
    {
        PaddedStringTable = NULL;
    }

    CheckSum += Length;
    OutOptHeader->CheckSum = CheckSum;

    if (fwrite(OutHeader, 1, StartOfRawData, OutFile) != StartOfRawData)
    {
        perror("Error writing output header\n");
        free(OutHeader);
        return 1;
    }

    for (Section = 0; Section < OutFileHeader->NumberOfSections; Section++)
    {
        if (OutSectionHeaders[Section].SizeOfRawData != 0)
        {
            DWORD SizeOfRawData;
            fseek(OutFile, OutSectionHeaders[Section].PointerToRawData, SEEK_SET);
            if (OutRelocSection == OutSectionHeaders + Section)
            {
                Data = (void *) ProcessedRelocs;
                SizeOfRawData = ProcessedRelocsLength;
            }
            else if (RosSymLength > 0 && Section + 1 == OutFileHeader->NumberOfSections)
            {
                Data = (void *) PaddedRosSym;
                SizeOfRawData = OutSectionHeaders[Section].SizeOfRawData;
            }
            else
            {
                Data = (void *) ((char *) InData + OutSectionHeaders[Section].PointerToRawData);
                SizeOfRawData = OutSectionHeaders[Section].SizeOfRawData;
            }
            if (fwrite(Data, 1, SizeOfRawData, OutFile) != SizeOfRawData)
            {
                perror("Error writing section data\n");
                free(PaddedRosSym);
                free(OutHeader);
                return 1;
            }
        }
    }

    if (PaddedStringTable)
    {
        fseek(OutFile, OutFileHeader->PointerToSymbolTable, SEEK_SET);
        fwrite(PaddedStringTable, 1, PaddedStringTableLength, OutFile);
        free(PaddedStringTable);
    }

    if (PaddedRosSym)
    {
        free(PaddedRosSym);
    }
    free(OutHeader);

    return 0;
}

int main(int argc, char* argv[])
{
    PSYMBOLFILE_HEADER SymbolFileHeader;
    PIMAGE_DOS_HEADER PEDosHeader;
    PIMAGE_FILE_HEADER PEFileHeader;
    PIMAGE_OPTIONAL_HEADER PEOptHeader;
    PIMAGE_SECTION_HEADER PESectionHeaders;
    ULONG ImageBase;
    void *StabBase;
    ULONG StabsLength;
    void *StabStringBase;
    ULONG StabStringsLength;
    void *CoffBase = NULL;
    ULONG CoffsLength;
    void *CoffStringBase = NULL;
    ULONG CoffStringsLength;
    char* path1;
    char* path2;
    FILE* out;
    void *StringBase = NULL;
    ULONG StringsLength = 0;
    ULONG StabSymbolsCount = 0;
    PROSSYM_ENTRY StabSymbols = NULL;
    ULONG CoffSymbolsCount = 0;
    PROSSYM_ENTRY CoffSymbols = NULL;
    ULONG MergedSymbolsCount = 0;
    PROSSYM_ENTRY MergedSymbols = NULL;
    size_t FileSize;
    void *FileData;
    ULONG RosSymLength;
    void *RosSymSection;
    DWORD module_base;
    void *file;
    char elfhdr[4] = { '\177', 'E', 'L', 'F' };
    BOOLEAN UseDbgHelp = FALSE;
    int arg, argstate = 0;
    char *SourcePath = NULL;

    for (arg = 1; arg < argc; arg++)
    {
        switch (argstate)
        {
            default:
                argstate = -1;
                break;

            case 0:
                if (!strcmp(argv[arg], "-s"))
                {
                    argstate = 1;
                }
                else
                {
                    argstate = 2;
                    path1 = convert_path(argv[arg]);
                }
            break;

            case 1:
                free(SourcePath);
                SourcePath = strdup(argv[arg]);
                argstate = 0;
                break;

            case 2:
                path2 = convert_path(argv[arg]);
                argstate = 3;
                break;
        }
    }

    if (argstate != 3)
    {
        fprintf(stderr, "Usage: rsym [-s <sources>] <input> <output>\n");
        exit(1);
    }

    FileData = load_file(path1, &FileSize);
    if (!FileData)
    {
        fprintf(stderr, "An error occured loading '%s'\n", path1);
        exit(1);
    }

    file = fopen(path1, "rb");

    /* Check if MZ header exists  */
    PEDosHeader = (PIMAGE_DOS_HEADER) FileData;
    if (PEDosHeader->e_magic != IMAGE_DOS_MAGIC ||
        PEDosHeader->e_lfanew == 0L)
    {
        /* Ignore elf */
        if (!memcmp(PEDosHeader, elfhdr, sizeof(elfhdr)))
            exit(0);
        perror("Input file is not a PE image.\n");
        free(FileData);
        exit(1);
    }

    /* Locate PE file header  */
    /* sizeof(ULONG) = sizeof(MAGIC) */
    PEFileHeader = (PIMAGE_FILE_HEADER)((char *) FileData + PEDosHeader->e_lfanew + sizeof(ULONG));

    /* Locate optional header */
    assert(sizeof(ULONG) == 4);
    PEOptHeader = (PIMAGE_OPTIONAL_HEADER)(PEFileHeader + 1);
    ImageBase = PEOptHeader->ImageBase;

    /* Locate PE section headers  */
    PESectionHeaders = (PIMAGE_SECTION_HEADER)((char *) PEOptHeader + PEFileHeader->SizeOfOptionalHeader);

    if (GetStabInfo(FileData,
                    PEFileHeader,
                    PESectionHeaders,
                    &StabsLength,
                    &StabBase,
                    &StabStringsLength,
                    &StabStringBase))
    {
        free(FileData);
        exit(1);
    }

    if (StabsLength == 0)
    {
        // SYMOPT_AUTO_PUBLICS
        // SYMOPT_FAVOR_COMPRESSED
        // SYMOPT_LOAD_ANYTHING
        // SYMOPT_LOAD_LINES
        SymSetOptions(0x10000 | 0x800000 | 0x40 | 0x10);
        SymInitialize(FileData, ".", 0);

        module_base = SymLoadModule(FileData, file, path1, path1, 0, FileSize) & 0xffffffff;

        if (ConvertDbgHelp(FileData,
                           module_base,
                           SourcePath,
                           &StabSymbolsCount,
                           &StabSymbols,
                           &StringsLength,
                           &StringBase))
        {
            free(FileData);
            exit(1);
        }

        UseDbgHelp = TRUE;
        SymUnloadModule(FileData, module_base);
        SymCleanup(FileData);
    }

    if (GetCoffInfo(FileData,
                    PEFileHeader,
                    PESectionHeaders,
                    &CoffsLength,
                    &CoffBase,
                    &CoffStringsLength,
                    &CoffStringBase))
    {
        free(FileData);
        exit(1);
    }

    if (!UseDbgHelp)
    {
        StringBase = malloc(1 + StringsLength + CoffStringsLength +
                            (CoffsLength / sizeof(ROSSYM_ENTRY)) * (E_SYMNMLEN + 1));
        if (StringBase == NULL)
        {
            free(FileData);
            fprintf(stderr, "Failed to allocate memory for strings table\n");
            exit(1);
        }
        /* Make offset 0 into an empty string */
        *((char *) StringBase) = '\0';
        StringsLength = 1;

        if (ConvertStabs(&StabSymbolsCount,
                         &StabSymbols,
                         &StringsLength,
                         StringBase,
                         StabsLength,
                         StabBase,
                         StabStringsLength,
                         StabStringBase,
                         ImageBase,
                         PEFileHeader,
                         PESectionHeaders))
        {
            free(StringBase);
            free(FileData);
            fprintf(stderr, "Failed to allocate memory for strings table\n");
            exit(1);
        }
    }
    else
    {
        StringBase = realloc(StringBase, StringsLength + CoffStringsLength);
        if (!StringBase)
        {
            free(FileData);
            fprintf(stderr, "Failed to allocate memory for strings table\n");
            exit(1);
        }
    }

    if (ConvertCoffs(&CoffSymbolsCount,
                     &CoffSymbols,
                     &StringsLength,
                     StringBase,
                     CoffsLength,
                     CoffBase,
                     CoffStringsLength,
                     CoffStringBase,
                     ImageBase,
                     PEFileHeader,
                     PESectionHeaders))
    {
        if (StabSymbols)
        {
            free(StabSymbols);
        }
        free(StringBase);
        free(FileData);
        exit(1);
    }

    if (MergeStabsAndCoffs(&MergedSymbolsCount,
                           &MergedSymbols,
                           StabSymbolsCount,
                           StabSymbols,
                           CoffSymbolsCount,
                           CoffSymbols))
    {
        if (CoffSymbols)
        {
            free(CoffSymbols);
        }
        if (StabSymbols)
        {
            free(StabSymbols);
        }
        free(StringBase);
        free(FileData);
        exit(1);
    }

    if (CoffSymbols)
    {
        free(CoffSymbols);
    }
    if (StabSymbols)
    {
        free(StabSymbols);
    }
    if (MergedSymbolsCount == 0)
    {
        RosSymLength = 0;
        RosSymSection = NULL;
    }
    else
    {
        RosSymLength = sizeof(SYMBOLFILE_HEADER) +
                       MergedSymbolsCount * sizeof(ROSSYM_ENTRY) +
                       StringsLength;

        RosSymSection = malloc(RosSymLength);
        if (RosSymSection == NULL)
        {
            free(MergedSymbols);
            free(StringBase);
            free(FileData);
            fprintf(stderr, "Unable to allocate memory for .rossym section\n");
            exit(1);
        }
        memset(RosSymSection, '\0', RosSymLength);

        SymbolFileHeader = (PSYMBOLFILE_HEADER)RosSymSection;
        SymbolFileHeader->SymbolsOffset = sizeof(SYMBOLFILE_HEADER);
        SymbolFileHeader->SymbolsLength = MergedSymbolsCount * sizeof(ROSSYM_ENTRY);
        SymbolFileHeader->StringsOffset = SymbolFileHeader->SymbolsOffset +
                                          SymbolFileHeader->SymbolsLength;
        SymbolFileHeader->StringsLength = StringsLength;

        memcpy((char *) RosSymSection + SymbolFileHeader->SymbolsOffset,
               MergedSymbols,
               SymbolFileHeader->SymbolsLength);

        memcpy((char *) RosSymSection + SymbolFileHeader->StringsOffset,
               StringBase,
               SymbolFileHeader->StringsLength);

        free(MergedSymbols);
    }

    free(StringBase);
    out = fopen(path2, "wb");
    if (out == NULL)
    {
        perror("Cannot open output file");
        free(RosSymSection);
        free(FileData);
        exit(1);
    }

    if (CreateOutputFile(out,
                         FileData,
                         PEDosHeader,
                         PEFileHeader,
                         PEOptHeader,
                         PESectionHeaders,
                         RosSymLength,
                         RosSymSection))
    {
        fclose(out);
        if (RosSymSection)
        {
            free(RosSymSection);
        }
        free(FileData);
        exit(1);
    }

    fclose(out);
    if (RosSymSection)
    {
        free(RosSymSection);
    }
    free(FileData);

    return 0;
}

/* EOF */
