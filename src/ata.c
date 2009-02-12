// Low level ATA disk access
//
// Copyright (C) 2008  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "types.h" // u8
#include "ioport.h" // inb
#include "util.h" // dprintf
#include "cmos.h" // inb_cmos
#include "pic.h" // enable_hwirq
#include "biosvar.h" // GET_EBDA
#include "pci.h" // pci_find_class
#include "pci_ids.h" // PCI_CLASS_STORAGE_OTHER
#include "pci_regs.h" // PCI_INTERRUPT_LINE
#include "boot.h" // add_bcv_hd
#include "disk.h" // struct ata_s
#include "atabits.h" // ATA_CB_STAT

#define TIMEOUT 0
#define BSY 1
#define NOT_BSY 2
#define NOT_BSY_DRQ 3
#define NOT_BSY_NOT_DRQ 4
#define NOT_BSY_RDY 5

#define IDE_SECTOR_SIZE 512
#define CDROM_SECTOR_SIZE 2048

#define IDE_TIMEOUT 32000u //32 seconds max for IDE ops

struct ata_s ATA VAR16_32;


/****************************************************************
 * Helper functions
 ****************************************************************/

// Wait for the specified ide state
static inline int
await_ide(u8 mask, u8 flags, u16 base, u16 timeout)
{
    u64 end = calc_future_tsc(timeout);
    for (;;) {
        u8 status = inb(base+ATA_CB_STAT);
        if ((status & mask) == flags)
            return status;
        if (rdtscll() >= end) {
            dprintf(1, "IDE time out\n");
            return -1;
        }
    }
}

// Wait for the device to be not-busy.
static int
await_not_bsy(u16 base)
{
    return await_ide(ATA_CB_STAT_BSY, 0, base, IDE_TIMEOUT);
}

// Wait for the device to be ready.
static int
await_rdy(u16 base)
{
    return await_ide(ATA_CB_STAT_RDY, ATA_CB_STAT_RDY, base, IDE_TIMEOUT);
}

// Wait for ide state - pauses for one ata cycle first.
static __always_inline int
pause_await_not_bsy(u16 iobase1, u16 iobase2)
{
    // Wait one PIO transfer cycle.
    inb(iobase2 + ATA_CB_ASTAT);

    return await_not_bsy(iobase1);
}

// Wait for ide state - pause for 400ns first.
static __always_inline int
ndelay_await_not_bsy(u16 iobase1)
{
    ndelay(400);
    return await_not_bsy(iobase1);
}

// Reset a drive
void
ata_reset(int driveid)
{
    u8 channel = driveid / 2;
    u8 slave = driveid % 2;
    u16 iobase1 = GET_GLOBAL(ATA.channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA.channels[channel].iobase2);

    dprintf(6, "ata_reset driveid=%d\n", driveid);
    // Pulse SRST
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN | ATA_CB_DC_SRST, iobase2+ATA_CB_DC);
    udelay(5);
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN, iobase2+ATA_CB_DC);
    mdelay(2);

    // wait for device to become not busy.
    int status = await_not_bsy(iobase1);
    if (status < 0)
        goto done;
    if (slave) {
        // Change device.
        u64 end = calc_future_tsc(IDE_TIMEOUT);
        for (;;) {
            outb(ATA_CB_DH_DEV1, iobase1 + ATA_CB_DH);
            status = await_not_bsy(iobase1);
            if (status < 0)
                goto done;
            if (inb(iobase1 + ATA_CB_DH) == ATA_CB_DH_DEV1)
                break;
            // Change drive request failed to take effect - retry.
            if (rdtscll() >= end) {
                dprintf(1, "ata_reset slave time out\n");
                goto done;
            }
        }
    }

    // On a user-reset request, wait for RDY if it is an ATA device.
    u8 type=GET_GLOBAL(ATA.devices[driveid].type);
    if (type == ATA_TYPE_ATA)
        status = await_rdy(iobase1);

done:
    // Enable interrupts
    outb(ATA_CB_DC_HD15, iobase2+ATA_CB_DC);

    dprintf(6, "ata_reset exit status=%x\n", status);
}


/****************************************************************
 * ATA send command
 ****************************************************************/

struct ata_pio_command {
    u8 feature;
    u8 sector_count;
    u8 lba_low;
    u8 lba_mid;
    u8 lba_high;
    u8 device;
    u8 command;

    u8 sector_count2;
    u8 lba_low2;
    u8 lba_mid2;
    u8 lba_high2;
};

// Send an ata command to the drive.
static int
send_cmd(int driveid, struct ata_pio_command *cmd)
{
    u8 channel = driveid / 2;
    u8 slave = driveid % 2;
    u16 iobase1 = GET_GLOBAL(ATA.channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA.channels[channel].iobase2);

    // Disable interrupts
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN, iobase2 + ATA_CB_DC);

    // Select device
    int status = await_not_bsy(iobase1);
    if (status < 0)
        return status;
    u8 newdh = ((cmd->device & ~ATA_CB_DH_DEV1)
                | (slave ? ATA_CB_DH_DEV1 : ATA_CB_DH_DEV0));
    u8 olddh = inb(iobase1 + ATA_CB_DH);
    outb(newdh, iobase1 + ATA_CB_DH);
    if ((olddh ^ newdh) & (1<<4)) {
        // Was a device change - wait for device to become not busy.
        status = await_not_bsy(iobase1);
        if (status < 0)
            return status;
    }

    if (cmd->command & 0x04) {
        outb(0x00, iobase1 + ATA_CB_FR);
        outb(cmd->sector_count2, iobase1 + ATA_CB_SC);
        outb(cmd->lba_low2, iobase1 + ATA_CB_SN);
        outb(cmd->lba_mid2, iobase1 + ATA_CB_CL);
        outb(cmd->lba_high2, iobase1 + ATA_CB_CH);
    }
    outb(cmd->feature, iobase1 + ATA_CB_FR);
    outb(cmd->sector_count, iobase1 + ATA_CB_SC);
    outb(cmd->lba_low, iobase1 + ATA_CB_SN);
    outb(cmd->lba_mid, iobase1 + ATA_CB_CL);
    outb(cmd->lba_high, iobase1 + ATA_CB_CH);
    outb(cmd->command, iobase1 + ATA_CB_CMD);

    status = ndelay_await_not_bsy(iobase1);
    if (status < 0)
        return status;

    if (status & ATA_CB_STAT_ERR) {
        dprintf(6, "send_cmd : read error (status=%02x err=%02x)\n"
                , status, inb(iobase1 + ATA_CB_ERR));
        return -4;
    }
    if (!(status & ATA_CB_STAT_DRQ)) {
        dprintf(6, "send_cmd : DRQ not set (status %02x)\n", status);
        return -5;
    }

    return 0;
}


/****************************************************************
 * ATA transfers
 ****************************************************************/

// Read and discard x number of bytes from an io channel.
static void
insx_discard(int mode, int iobase1, int bytes)
{
    int count, i;
    if (mode == ATA_MODE_PIO32) {
        count = bytes / 4;
        for (i=0; i<count; i++)
            inl(iobase1);
    } else {
        count = bytes / 2;
        for (i=0; i<count; i++)
            inw(iobase1);
    }
}

// Transfer 'count' blocks (of 'blocksize' bytes) to/from drive
// 'driveid'.  If 'skipfirst' or 'skiplast' is set then the first
// and/or last block may be partially transferred.  This function is
// inlined because all the callers use different forms and because the
// large number of parameters would consume a lot of stack space.
static __always_inline int
ata_transfer(int driveid, int iswrite, int count, int blocksize
             , int skipfirst, int skiplast, void *buf_fl)
{
    dprintf(16, "ata_transfer id=%d write=%d count=%d bs=%d"
            " skipf=%d skipl=%d buf=%p\n"
            , driveid, iswrite, count, blocksize
            , skipfirst, skiplast, buf_fl);

    // Reset count of transferred data
    SET_EBDA(sector_count, 0);

    u8 channel  = driveid / 2;
    u16 iobase1 = GET_GLOBAL(ATA.channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA.channels[channel].iobase2);
    u8 mode     = GET_GLOBAL(ATA.devices[driveid].mode);
    int current = 0;
    int status;
    for (;;) {
        int bsize = blocksize;
        if (skipfirst && current == 0) {
            insx_discard(mode, iobase1, skipfirst);
            bsize -= skipfirst;
        }
        if (skiplast && current == count-1)
            bsize -= skiplast;

        if (iswrite) {
            // Write data to controller
            dprintf(16, "Write sector id=%d dest=%p\n", driveid, buf_fl);
            if (mode == ATA_MODE_PIO32)
                outsl_fl(iobase1, buf_fl, bsize / 4);
            else
                outsw_fl(iobase1, buf_fl, bsize / 2);
        } else {
            // Read data from controller
            dprintf(16, "Read sector id=%d dest=%p\n", driveid, buf_fl);
            if (mode == ATA_MODE_PIO32)
                insl_fl(iobase1, buf_fl, bsize / 4);
            else
                insw_fl(iobase1, buf_fl, bsize / 2);
        }
        buf_fl += bsize;

        if (skiplast && current == count-1)
            insx_discard(mode, iobase1, skiplast);

        status = pause_await_not_bsy(iobase1, iobase2);
        if (status < 0)
            // Error
            return status;

        current++;
        SET_EBDA(sector_count, current);
        if (current == count)
            break;
        status &= (ATA_CB_STAT_BSY | ATA_CB_STAT_DRQ | ATA_CB_STAT_ERR);
        if (status != ATA_CB_STAT_DRQ) {
            dprintf(6, "ata_transfer : more sectors left (status %02x)\n"
                    , status);
            return -6;
        }
    }

    status &= (ATA_CB_STAT_BSY | ATA_CB_STAT_DF | ATA_CB_STAT_DRQ
               | ATA_CB_STAT_ERR);
    if (!iswrite)
        status &= ~ATA_CB_STAT_DF;
    if (status != 0) {
        dprintf(6, "ata_transfer : no sectors left (status %02x)\n", status);
        return -7;
    }

    // Enable interrupts
    outb(ATA_CB_DC_HD15, iobase2+ATA_CB_DC);
    return 0;
}

static noinline int
ata_transfer_disk(const struct disk_op_s *op)
{
    return ata_transfer(op->driveid, op->command == ATA_CMD_WRITE_SECTORS
                        , op->count, IDE_SECTOR_SIZE, 0, 0, op->buf_fl);
}

static noinline int
ata_transfer_cdrom(const struct disk_op_s *op)
{
    return ata_transfer(op->driveid, 0, op->count, CDROM_SECTOR_SIZE
                        , 0, 0, op->buf_fl);
}

static noinline int
ata_transfer_cdemu(const struct disk_op_s *op, int before, int after)
{
    int vcount = op->count * 4 - before - after;
    int ret = ata_transfer(op->driveid, 0, op->count, CDROM_SECTOR_SIZE
                           , before*512, after*512, op->buf_fl);
    if (ret) {
        SET_EBDA(sector_count, 0);
        return ret;
    }
    SET_EBDA(sector_count, vcount);
    return 0;
}


/****************************************************************
 * ATA hard drive functions
 ****************************************************************/

static noinline int
send_cmd_disk(const struct disk_op_s *op)
{
    u64 lba = op->lba;

    struct ata_pio_command cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.command = op->command;
    if (op->count >= (1<<8) || lba + op->count >= (1<<28)) {
        cmd.sector_count2 = op->count >> 8;
        cmd.lba_low2 = lba >> 24;
        cmd.lba_mid2 = lba >> 32;
        cmd.lba_high2 = lba >> 40;

        cmd.command |= 0x04;
        lba &= 0xffffff;
    }

    cmd.feature = 0;
    cmd.sector_count = op->count;
    cmd.lba_low = lba;
    cmd.lba_mid = lba >> 8;
    cmd.lba_high = lba >> 16;
    cmd.device = ((lba >> 24) & 0xf) | ATA_CB_DH_LBA;

    return send_cmd(op->driveid, &cmd);
}

// Read/write count blocks from a harddrive.
__always_inline int
ata_cmd_data(struct disk_op_s *op)
{
    int ret = send_cmd_disk(op);
    if (ret)
        return ret;
    return ata_transfer_disk(op);
}


/****************************************************************
 * ATAPI functions
 ****************************************************************/

// Low-level atapi command transmit function.
static __always_inline int
send_atapi_cmd(int driveid, u8 *cmdbuf, u8 cmdlen, u16 blocksize)
{
    u8 channel = driveid / 2;
    u16 iobase1 = GET_GLOBAL(ATA.channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA.channels[channel].iobase2);

    struct ata_pio_command cmd;
    cmd.sector_count = 0;
    cmd.feature = 0;
    cmd.lba_low = 0;
    cmd.lba_mid = blocksize;
    cmd.lba_high = blocksize >> 8;
    cmd.device = 0;
    cmd.command = ATA_CMD_PACKET;

    int ret = send_cmd(driveid, &cmd);
    if (ret)
        return ret;

    // Send command to device
    outsw_fl(iobase1, MAKE_FLATPTR(GET_SEG(SS), cmdbuf), cmdlen / 2);

    int status = pause_await_not_bsy(iobase1, iobase2);
    if (status < 0)
        return status;

    if (status & ATA_CB_STAT_ERR) {
        dprintf(6, "send_atapi_cmd : read error (status=%02x err=%02x)\n"
                , status, inb(iobase1 + ATA_CB_ERR));
        return -2;
    }
    if (!(status & ATA_CB_STAT_DRQ)) {
        dprintf(6, "send_atapi_cmd : DRQ not set (status %02x)\n", status);
        return -3;
    }

    return 0;
}

// Low-level cdrom read atapi command transmit function.
static int
send_cmd_cdrom(const struct disk_op_s *op)
{
    u8 atacmd[12];
    memset(atacmd, 0, sizeof(atacmd));

    atacmd[0]=0x28;                         // READ command
    atacmd[7]=(op->count & 0xff00) >> 8;    // Sectors
    atacmd[8]=(op->count & 0x00ff);
    atacmd[2]=(op->lba & 0xff000000) >> 24; // LBA
    atacmd[3]=(op->lba & 0x00ff0000) >> 16;
    atacmd[4]=(op->lba & 0x0000ff00) >> 8;
    atacmd[5]=(op->lba & 0x000000ff);

    return send_atapi_cmd(op->driveid, atacmd, sizeof(atacmd)
                          , CDROM_SECTOR_SIZE);
}

// Read sectors from the cdrom.
__always_inline int
cdrom_read(struct disk_op_s *op)
{
    int ret = send_cmd_cdrom(op);
    if (ret)
        return ret;

    return ata_transfer_cdrom(op);
}

// Pretend the cdrom has 512 byte sectors (instead of 2048) and read
// sectors.
__always_inline int
cdrom_read_512(struct disk_op_s *op)
{
    u32 vlba = op->lba;
    u32 vcount = op->count;
    u32 lba = op->lba = vlba / 4;
    u32 velba = vlba + vcount - 1;
    u32 elba = velba / 4;
    op->count = elba - lba + 1;
    int before = vlba % 4;
    int after = 3 - (velba % 4);

    dprintf(16, "cdrom_read_512: id=%d vlba=%d vcount=%d buf=%p lba=%d elba=%d"
            " count=%d before=%d after=%d\n"
            , op->driveid, vlba, vcount, op->buf_fl, lba, elba
            , op->count, before, after);

    int ret = send_cmd_cdrom(op);
    if (ret)
        return ret;

    return ata_transfer_cdemu(op, before, after);
}

// Send a simple atapi command to a drive.
int
ata_cmd_packet(int driveid, u8 *cmdbuf, u8 cmdlen
               , u32 length, void *buf_fl)
{
    int ret = send_atapi_cmd(driveid, cmdbuf, cmdlen, length);
    if (ret)
        return ret;

    return ata_transfer(driveid, 0, 1, length, 0, 0, buf_fl);
}


/****************************************************************
 * Disk geometry translation
 ****************************************************************/

static u8
get_translation(int driveid)
{
    if (! CONFIG_COREBOOT) {
        // Emulators pass in the translation info via nvram.
        u8 channel = driveid / 2;
        u8 translation = inb_cmos(CMOS_BIOS_DISKTRANSFLAG + channel/2);
        translation >>= 2 * (driveid % 4);
        translation &= 0x03;
        return translation;
    }

    // On COREBOOT, use a heuristic to determine translation type.
    u16 heads = GET_GLOBAL(ATA.devices[driveid].pchs.heads);
    u16 cylinders = GET_GLOBAL(ATA.devices[driveid].pchs.cylinders);
    u16 spt = GET_GLOBAL(ATA.devices[driveid].pchs.spt);

    if (cylinders <= 1024 && heads <= 16 && spt <= 63)
        return ATA_TRANSLATION_NONE;
    if (cylinders * heads <= 131072)
        return ATA_TRANSLATION_LARGE;
    return ATA_TRANSLATION_LBA;
}

static void
setup_translation(int driveid)
{
    u8 translation = get_translation(driveid);
    SET_GLOBAL(ATA.devices[driveid].translation, translation);

    u8 channel = driveid / 2;
    u8 slave = driveid % 2;
    u16 heads = GET_GLOBAL(ATA.devices[driveid].pchs.heads);
    u16 cylinders = GET_GLOBAL(ATA.devices[driveid].pchs.cylinders);
    u16 spt = GET_GLOBAL(ATA.devices[driveid].pchs.spt);
    u64 sectors = GET_GLOBAL(ATA.devices[driveid].sectors);

    dprintf(1, "ata%d-%d: PCHS=%u/%d/%d translation="
            , channel, slave, cylinders, heads, spt);
    switch (translation) {
    case ATA_TRANSLATION_NONE:
        dprintf(1, "none");
        break;
    case ATA_TRANSLATION_LBA:
        dprintf(1, "lba");
        spt = 63;
        if (sectors > 63*255*1024) {
            heads = 255;
            cylinders = 1024;
            break;
        }
        u32 sect = (u32)sectors / 63;
        heads = sect / 1024;
        if (heads>128)
            heads = 255;
        else if (heads>64)
            heads = 128;
        else if (heads>32)
            heads = 64;
        else if (heads>16)
            heads = 32;
        else
            heads = 16;
        cylinders = sect / heads;
        break;
    case ATA_TRANSLATION_RECHS:
        dprintf(1, "r-echs");
        // Take care not to overflow
        if (heads==16) {
            if (cylinders>61439)
                cylinders=61439;
            heads=15;
            cylinders = (u16)((u32)(cylinders)*16/15);
        }
        // then go through the large bitshift process
    case ATA_TRANSLATION_LARGE:
        if (translation == ATA_TRANSLATION_LARGE)
            dprintf(1, "large");
        while (cylinders > 1024) {
            cylinders >>= 1;
            heads <<= 1;

            // If we max out the head count
            if (heads > 127)
                break;
        }
        break;
    }
    // clip to 1024 cylinders in lchs
    if (cylinders > 1024)
        cylinders = 1024;
    dprintf(1, " LCHS=%d/%d/%d\n", cylinders, heads, spt);

    SET_GLOBAL(ATA.devices[driveid].lchs.heads, heads);
    SET_GLOBAL(ATA.devices[driveid].lchs.cylinders, cylinders);
    SET_GLOBAL(ATA.devices[driveid].lchs.spt, spt);
}


/****************************************************************
 * ATA detect and init
 ****************************************************************/

// Extract common information from IDENTIFY commands.
static void
extract_identify(int driveid, u16 *buffer)
{
    dprintf(3, "Identify w0=%x w2=%x\n", buffer[0], buffer[2]);

    // Read model name
    char *model = ATA.devices[driveid].model;
    int maxsize = ARRAY_SIZE(ATA.devices[driveid].model);
    int i;
    for (i=0; i<maxsize/2; i++) {
        u16 v = buffer[27+i];
        model[i*2] = v >> 8;
        model[i*2+1] = v & 0xff;
    }
    model[maxsize-1] = 0x00;

    // Trim trailing spaces from model name.
    for (i=maxsize-2; i>0 && model[i] == 0x20; i--)
        model[i] = 0x00;

    // Extract ATA/ATAPI version.
    u16 ataversion = buffer[80];
    u8 version;
    for (version=15; version>0; version--)
        if (ataversion & (1<<version))
            break;
    ATA.devices[driveid].version = version;

    // Common flags.
    SET_GLOBAL(ATA.devices[driveid].removable, (buffer[0] & 0x80) ? 1 : 0);
    // XXX - what is buffer[48]?
    SET_GLOBAL(ATA.devices[driveid].mode
               , buffer[48] ? ATA_MODE_PIO32 : ATA_MODE_PIO16);
}

static int
init_drive_atapi(int driveid)
{
    // Send an IDENTIFY_DEVICE_PACKET command to device
    u16 buffer[256];
    memset(buffer, 0, sizeof(buffer));
    struct disk_op_s dop;
    dop.driveid = driveid;
    dop.command = ATA_CMD_IDENTIFY_DEVICE_PACKET;
    dop.count = 1;
    dop.lba = 1;
    dop.buf_fl = MAKE_FLATPTR(GET_SEG(SS), buffer);
    int ret = ata_cmd_data(&dop);
    if (ret)
        return ret;

    // Success - setup as ATAPI.
    extract_identify(driveid, buffer);
    SET_GLOBAL(ATA.devices[driveid].type, ATA_TYPE_ATAPI);
    SET_GLOBAL(ATA.devices[driveid].device, (buffer[0] >> 8) & 0x1f);
    SET_GLOBAL(ATA.devices[driveid].blksize, CDROM_SECTOR_SIZE);

    // fill cdidmap
    u8 cdcount = GET_GLOBAL(ATA.cdcount);
    SET_GLOBAL(ATA.idmap[1][cdcount], driveid);
    SET_GLOBAL(ATA.cdcount, cdcount+1);

    // Report drive info to user.
    u8 channel = driveid / 2;
    u8 slave = driveid % 2;
    printf("ata%d-%d: %s ATAPI-%d %s\n", channel, slave
           , ATA.devices[driveid].model, ATA.devices[driveid].version
           , (ATA.devices[driveid].type == ATA_DEVICE_CDROM
              ? "CD-Rom/DVD-Rom" : "Device"));

    return 0;
}

static int
init_drive_ata(int driveid)
{
    // Send an IDENTIFY_DEVICE command to device
    u16 buffer[256];
    memset(buffer, 0, sizeof(buffer));
    struct disk_op_s dop;
    dop.driveid = driveid;
    dop.command = ATA_CMD_IDENTIFY_DEVICE;
    dop.count = 1;
    dop.lba = 1;
    dop.buf_fl = MAKE_FLATPTR(GET_SEG(SS), buffer);
    int ret = ata_cmd_data(&dop);
    if (ret)
        return ret;

    // Success - setup as ATA.
    extract_identify(driveid, buffer);
    SET_GLOBAL(ATA.devices[driveid].type, ATA_TYPE_ATA);
    SET_GLOBAL(ATA.devices[driveid].device, ATA_DEVICE_HD);
    SET_GLOBAL(ATA.devices[driveid].blksize, IDE_SECTOR_SIZE);

    SET_GLOBAL(ATA.devices[driveid].pchs.cylinders, buffer[1]);
    SET_GLOBAL(ATA.devices[driveid].pchs.heads, buffer[3]);
    SET_GLOBAL(ATA.devices[driveid].pchs.spt, buffer[6]);

    u64 sectors;
    if (buffer[83] & (1 << 10)) // word 83 - lba48 support
        sectors = *(u64*)&buffer[100]; // word 100-103
    else
        sectors = *(u32*)&buffer[60]; // word 60 and word 61
    SET_GLOBAL(ATA.devices[driveid].sectors, sectors);

    // Setup disk geometry translation.
    setup_translation(driveid);

    // Report drive info to user.
    u8 channel = driveid / 2;
    u8 slave = driveid % 2;
    char *model = ATA.devices[driveid].model;
    printf("ata%d-%d: %s ATA-%d Hard-Disk ", channel, slave, model
           , ATA.devices[driveid].version);
    u64 sizeinmb = sectors >> 11;
    if (sizeinmb < (1 << 16))
        printf("(%u MiBytes)\n", (u32)sizeinmb);
    else
        printf("(%u GiBytes)\n", (u32)(sizeinmb >> 10));

    // Register with bcv system.
    add_bcv_hd(driveid, model);

    return 0;
}

static void
ata_detect()
{
    // Device detection
    int driveid, last_reset_driveid=-1;
    for(driveid=0; driveid<CONFIG_MAX_ATA_DEVICES; driveid++) {
        u8 channel = driveid / 2;
        u8 slave = driveid % 2;

        u16 iobase1 = GET_GLOBAL(ATA.channels[channel].iobase1);
        if (!iobase1)
            break;

        // Look for device
        outb(slave ? ATA_CB_DH_DEV1 : ATA_CB_DH_DEV0, iobase1+ATA_CB_DH);
        outb(0x55, iobase1+ATA_CB_SC);
        outb(0xaa, iobase1+ATA_CB_SN);
        outb(0xaa, iobase1+ATA_CB_SC);
        outb(0x55, iobase1+ATA_CB_SN);
        outb(0x55, iobase1+ATA_CB_SC);
        outb(0xaa, iobase1+ATA_CB_SN);

        // Check if ioport registers look valid.
        u8 sc = inb(iobase1+ATA_CB_SC);
        u8 sn = inb(iobase1+ATA_CB_SN);
        dprintf(6, "ata_detect drive=%d sc=%x sn=%x\n", driveid, sc, sn);
        if (sc != 0x55 || sn != 0xaa)
            continue;

        // reset the channel
        if (slave && driveid == last_reset_driveid + 1) {
            // The drive was just reset - no need to reset it again.
        } else {
            ata_reset(driveid);
            last_reset_driveid = driveid;
        }

        // check for ATAPI
        int ret = init_drive_atapi(driveid);
        if (!ret)
            // Found an ATAPI drive.
            continue;

        u8 st = inb(iobase1+ATA_CB_STAT);
        if (!st)
            // Status not set - can't be a valid drive.
            continue;

        // Wait for RDY.
        ret = await_rdy(iobase1);
        if (ret < 0)
            continue;

        // check for ATA.
        init_drive_ata(driveid);
    }

    printf("\n");
}

static void
ata_init()
{
    memset(&ATA, 0, sizeof(ATA));

    // hdidmap and cdidmap init.
    u8 device;
    for (device=0; device < CONFIG_MAX_ATA_DEVICES; device++) {
        SET_GLOBAL(ATA.idmap[0][device], CONFIG_MAX_ATA_DEVICES);
        SET_GLOBAL(ATA.idmap[1][device], CONFIG_MAX_ATA_DEVICES);
    }

    // Scan PCI bus for ATA adapters
    int count=0;
    int bdf, max;
    foreachpci(bdf, max) {
        if (pci_config_readw(bdf, PCI_CLASS_DEVICE) != PCI_CLASS_STORAGE_IDE)
            continue;
        if (count >= ARRAY_SIZE(ATA.channels))
            break;

        u8 irq = pci_config_readb(bdf, PCI_INTERRUPT_LINE);
        SET_GLOBAL(ATA.channels[count].irq, irq);
        SET_GLOBAL(ATA.channels[count].pci_bdf, bdf);

        u8 prog_if = pci_config_readb(bdf, PCI_CLASS_PROG);
        u32 port1, port2;

        if (prog_if & 1) {
            port1 = pci_config_readl(bdf, PCI_BASE_ADDRESS_0) & ~3;
            port2 = pci_config_readl(bdf, PCI_BASE_ADDRESS_1) & ~3;
        } else {
            port1 = 0x1f0;
            port2 = 0x3f0;
        }
        SET_GLOBAL(ATA.channels[count].iobase1, port1);
        SET_GLOBAL(ATA.channels[count].iobase2, port2);
        dprintf(1, "ATA controller %d at %x/%x (dev %x prog_if %x)\n"
                , count, port1, port2, bdf, prog_if);
        count++;

        if (prog_if & 4) {
            port1 = pci_config_readl(bdf, PCI_BASE_ADDRESS_2) & ~3;
            port2 = pci_config_readl(bdf, PCI_BASE_ADDRESS_3) & ~3;
        } else {
            port1 = 0x170;
            port2 = 0x370;
        }
        dprintf(1, "ATA controller %d at %x/%x (dev %x prog_if %x)\n"
                , count, port1, port2, bdf, prog_if);
        SET_GLOBAL(ATA.channels[count].iobase1, port1);
        SET_GLOBAL(ATA.channels[count].iobase2, port2);
        count++;
    }
}

void
hard_drive_setup()
{
    if (!CONFIG_ATA)
        return;

    dprintf(3, "init hard drives\n");
    ata_init();
    ata_detect();

    SET_BDA(disk_control_byte, 0xc0);

    enable_hwirq(14, entry_76);
}


/****************************************************************
 * Drive mapping
 ****************************************************************/

// Fill in Fixed Disk Parameter Table (located in ebda).
static void
fill_fdpt(int driveid)
{
    if (driveid > 1)
        return;

    u16 nlc   = GET_GLOBAL(ATA.devices[driveid].lchs.cylinders);
    u16 nlh   = GET_GLOBAL(ATA.devices[driveid].lchs.heads);
    u16 nlspt = GET_GLOBAL(ATA.devices[driveid].lchs.spt);

    u16 npc   = GET_GLOBAL(ATA.devices[driveid].pchs.cylinders);
    u16 nph   = GET_GLOBAL(ATA.devices[driveid].pchs.heads);
    u16 npspt = GET_GLOBAL(ATA.devices[driveid].pchs.spt);

    struct fdpt_s *fdpt = &get_ebda_ptr()->fdpt[driveid];
    fdpt->precompensation = 0xffff;
    fdpt->drive_control_byte = 0xc0 | ((nph > 8) << 3);
    fdpt->landing_zone = npc;
    fdpt->cylinders = nlc;
    fdpt->heads = nlh;
    fdpt->sectors = nlspt;

    if (nlc == npc && nlh == nph && nlspt == npspt)
        // no logical CHS mapping used, just physical CHS
        // use Standard Fixed Disk Parameter Table (FDPT)
        return;

    // complies with Phoenix style Translated Fixed Disk Parameter
    // Table (FDPT)
    fdpt->phys_cylinders = npc;
    fdpt->phys_heads = nph;
    fdpt->phys_sectors = npspt;
    fdpt->a0h_signature = 0xa0;

    // Checksum structure.
    u8 sum = checksum((u8*)fdpt, sizeof(*fdpt)-1);
    fdpt->checksum = -sum;
}

// Map a drive (that was registered via add_bcv_hd)
void
map_drive(int driveid)
{
    // fill hdidmap
    u8 hdcount = GET_BDA(hdcount);
    dprintf(3, "Mapping driveid %d to %d\n", driveid, hdcount);
    SET_GLOBAL(ATA.idmap[0][hdcount], driveid);
    SET_BDA(hdcount, hdcount + 1);

    // Fill "fdpt" structure.
    fill_fdpt(hdcount);
}
