
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tbl.h"
#include "codec.h"
#include "../pflayer/pf.h"

#define SLOT_COUNT_OFFSET 2
#define SLOT_ENTRY_SIZE 4 /* 2 bytes offset + 2 bytes length */
#define PAGE_HEADER_SIZE 4

#define checkerr(err)           \
    {                           \
        if (err < 0)            \
        {                       \
            PF_PrintError();    \
            exit(EXIT_FAILURE); \
        }                       \
    }
static int slotDirAddr(int slot)
{
    return PF_PAGE_SIZE - (slot + 1) * SLOT_ENTRY_SIZE;
}

int getLen(int slot, byte *pageBuf)
{
    return (int)DecodeShort(pageBuf + slotDirAddr(slot) + 2);
}
void setLen(int slot, byte *pageBuf, int len)
{
    EncodeShort((short)len, pageBuf + slotDirAddr(slot) + 2);
}

int getNumSlots(byte *pageBuf)
{
    return (int)DecodeShort(pageBuf + SLOT_COUNT_OFFSET);
}
void setNumSlots(byte *pageBuf, int nslots)
{
    EncodeShort((short)nslots, pageBuf + SLOT_COUNT_OFFSET);
}

int getNthSlotOffset(int slot, char *pageBuf)
{
    return (int)DecodeShort((byte *)pageBuf + slotDirAddr(slot));
}
void setNthSlotOffset(int slot, byte *pageBuf, int offset)
{
    EncodeShort((short)offset, pageBuf + slotDirAddr(slot));
}

int getFreeSpaceOffset(byte *pageBuf)
{
    return (int)DecodeShort(pageBuf + 0);
}
void setFreeSpaceOffset(byte *pageBuf, int offset)
{
    EncodeShort((short)offset, pageBuf + 0);
}

/**
   Opens a paged file, creating one if it doesn't exist, and optionally
   overwriting it.
   Returns 0 on success and a negative error code otherwise.
   If successful, it returns an initialized Table*.
 */
int Table_Open(char *dbname, Schema *schema, bool overwrite, Table **ptable)
{
    // UNIMPLEMENTED;
    // Initialize PF, create PF file,
    // allocate Table structure  and initialize and return via ptable
    // The Table structure only stores the schema. The current functionality
    // does not really need the schema, because we are only concentrating
    // on record storage.
    int fd, return_code;

    if (dbname == NULL || ptable == NULL || schema == NULL)
    {
        return PFE_UNIX;
    }

    if (overwrite)
    {
        PF_DestroyFile(dbname); // createfile specifically asks that the file should not have been created before
    }

    return_code = PF_CreateFile(dbname);
    fd = PF_OpenFile(dbname);
    if (fd < 0)
    {
        // error  in opening file
        return fd;
    }

    Table *table = malloc(sizeof(Table));
    if (table == NULL)
    {
        PF_CloseFile(fd); // takes and fd argument to close
        return PFE_NOMEM;
    }
    table->fd = fd;
    table->schema = schema;
    strncpy(table->fileName, dbname, MAX_FNAME_LENGTH - 1);
    table->fileName[MAX_FNAME_LENGTH - 1] = '\0';

    *ptable = table; // return the table only if creation was successfull.

    return PFE_OK;
}

void Table_Close(Table *tbl)
{

    // Unfix any dirty pages, close file.
    // Unfix done properly in insert and get functions
    if (tbl == NULL)
    {
        return;
    }
    PF_CloseFile(tbl->fd);
    free(tbl);
}

int Table_Insert(Table *tbl, byte *record, int len, RecId *rid)
{
    // Allocate a fresh page if len is not enough for remaining space
    // Get the next free slot on page, and copy record in the free
    // space
    // Update slot and free space index information on top of page.
    int pageNum, err;
    byte *pageBuf;
    bool found = false;

    err = PF_GetFirstPage(tbl->fd, &pageNum, (char **)&pageBuf);
    while (err == PFE_OK)
    {
        int numSlots = getNumSlots(pageBuf);
        int freeSpaceOffset = getFreeSpaceOffset(pageBuf);
        int slotDirStart = PF_PAGE_SIZE - numSlots * SLOT_ENTRY_SIZE;
        int available = slotDirStart - freeSpaceOffset - SLOT_ENTRY_SIZE;

        if (available >= len)
        {
            found = true;
            break;
        }

        PF_UnfixPage(tbl->fd, pageNum, FALSE);
        err = PF_GetNextPage(tbl->fd, &pageNum, (char **)&pageBuf);
    }

    if (!found)
    {
        if (err != PFE_EOF)
        {
            return err; // a real error instead of "no page had space"
        }
        err = PF_AllocPage(tbl->fd, &pageNum, (char **)&pageBuf);
        if (err != PFE_OK)
        {
            return err;
        }
        setFreeSpaceOffset(pageBuf, PAGE_HEADER_SIZE);
        setNumSlots(pageBuf, 0);
    }

    int numSLots = getNumSlots(pageBuf);
    int freeSpaceOffset = getFreeSpaceOffset(pageBuf);

    memcpy(pageBuf + freeSpaceOffset, record, len);
    setNthSlotOffset(numSLots, pageBuf, freeSpaceOffset);
    setLen(numSLots, pageBuf, len);
    setNumSlots(pageBuf, numSLots + 1);
    setFreeSpaceOffset(pageBuf, freeSpaceOffset + len);

    *rid = (pageNum << 16) | numSLots;
    return PF_UnfixPage(tbl->fd, pageNum, TRUE);
}

/*
  Given an rid, fill in the record (but at most maxlen bytes).
  Returns the number of bytes copied.
 */
int Table_Get(Table *tbl, RecId rid, byte *record, int maxlen)
{

    // PF_GetThisPage(pageNum)
    // In the page get the slot offset of the record, and
    // memcpy bytes into the record supplied.
    // Unfix the page

    int slot = rid & 0xFFFF;
    int pageNum = rid >> 16;
    byte *pageBuf;

    int err = PF_GetThisPage(tbl->fd, pageNum, (char **)&pageBuf);
    if (err != PFE_OK)
    {
        return err;
    }
    int offset = getNthSlotOffset(slot, (char *)pageBuf);
    int len = getLen(slot, pageBuf);
    int copylen = (len < maxlen) ? len : maxlen;

    memcpy(record, pageBuf + offset, copylen);
    PF_UnfixPage(tbl->fd, pageNum, FALSE);

    return copylen; // return size of record
}

void Table_Scan(Table *tbl, void *callbackObj, ReadFunc callbackfn)
{
    // For each page obtained using PF_GetFirstPage and PF_GetNextPage
    //    for each record in that page,
    //          callbackfn(callbackObj, rid, record, recordLen)

    int pageNum, err;
    byte *pageBuf;

    err = PF_GetFirstPage(tbl->fd, &pageNum, (char **)&pageBuf);
    while (err == PFE_OK)
    {
        int numSlots = getNumSlots(pageBuf);
        for (int slot = 0; slot < numSlots; slot++)
        {
            int len = getLen(slot, pageBuf);
            int offset = getNthSlotOffset(slot, (char *)pageBuf);
            RecId rid = (pageNum << 16) | (slot & 0xFFFF);
            callbackfn(callbackObj, rid, (byte *)(pageBuf + offset), len);
        }
        PF_UnfixPage(tbl->fd, pageNum, FALSE);
        err = PF_GetNextPage(tbl->fd, &pageNum, (char **)&pageBuf);
    }
}
