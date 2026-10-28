/* 
 *  Arnold emulator (c) Copyright, Kevin Thacker 1995-2001
 *  
 *  This file is part of the Arnold emulator source code distribution.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
/*
        DISK IMAGE HANDLING CODE

        If disk image is single sided, and access made to 2nd side, currently
        forces to side 0.

*/
#include "../fdd.h"
#include "diskimg.h"
#include "dsk.h"
#include "extdsk.h"
#include "iextdsk.h"
#include "../host.h"
#include "../device.h"
#include "../cpc.h"

static DISKIMAGE_UNIT   Drives[4];

static void DiskImage_ImageRecognised(int DriveID,int Type)
{
        DISKIMAGE_UNIT  *pDrive=&Drives[DriveID];

        pDrive->Flags |= DISKIMAGE_DISK_INSERTED;
        pDrive->nImageType = Type;
}

void    DiskImage_Initialise(void)
{
        memset(Drives,0,sizeof(DISKIMAGE_UNIT));
}

void    DiskImage_Finish(void)
{
        DiskImage_RemoveDisk(0);
		FDD_InsertDisk(0,FALSE);

        DiskImage_RemoveDisk(1);
		FDD_InsertDisk(1,FALSE);
}

/*********************************************************************************/

int		DiskImage_InsertUnformattedDisk(int DriveID)
{
	/* remove existing disk */
	DiskImage_RemoveDisk(DriveID);

    DiskImage_ImageRecognised(DriveID,DISK_IMAGE_TYPE_UNDEFINED);
                            
    ExtDskInternal_Initialise(&Drives[DriveID]);
 
	Drives[DriveID].Flags |= DISKIMAGE_DISK_DIRTY;

	FDD_InsertDisk(DriveID, TRUE);
	return TRUE;
}

/* install a disk image into the drive */

/* pDiskImage is pointer to disk image data loaded from host */
/* DiskImageLength is length of data loaded from host */
/* assumes therefore that disc image data is contained in a single file */
int     DiskImage_InsertDisk(int DriveID, const unsigned char *pDiskImage, const unsigned long DiskImageLength)
{
 	/* validate disk image */
	if (Dsk_Validate(pDiskImage, DiskImageLength))
	{
		/* standard dsk */
		DiskImage_RemoveDisk(DriveID);

		/* valid */
		DiskImage_ImageRecognised(DriveID,DISK_IMAGE_TYPE_STANDARD);

		ExtDskInternal_Dsk2ExtDskInternal(&Drives[DriveID],pDiskImage, DiskImageLength);
		FDD_FitDiskTracks(DriveID, pDiskImage[0x30]);   /* tracks in the image header (both formats) */

		FDD_InsertDisk(DriveID, TRUE);
		return ARNOLD_STATUS_OK;
	}
	else
	if (ExtDsk_Validate(pDiskImage,DiskImageLength))
	{
		/* extdsk */
		DiskImage_RemoveDisk(DriveID);

		DiskImage_ImageRecognised(DriveID,DISK_IMAGE_TYPE_EXTENDED);

		ExtDskInternal_ExtDsk2ExtDskInternal(&Drives[DriveID],pDiskImage, DiskImageLength);
		FDD_FitDiskTracks(DriveID, pDiskImage[0x30]);   /* tracks in the image header (both formats) */

		FDD_InsertDisk(DriveID, TRUE);
		return ARNOLD_STATUS_OK;
	}
	else
	if (Dif_Validate(pDiskImage,DiskImageLength))
	{
		/* dif */
		DiskImage_RemoveDisk(DriveID);

		DiskImage_ImageRecognised(DriveID,DISK_IMAGE_TYPE_DIF);

		ExtDskInternal_Dif2ExtDskInternal(&Drives[DriveID],pDiskImage,DiskImageLength);

		FDD_InsertDisk(DriveID, TRUE);
		return ARNOLD_STATUS_OK;
	}
    return ARNOLD_STATUS_UNRECOGNISED;
}

/* remove a disk image from the drive */
void    DiskImage_RemoveDisk(int DriveID)
{
        DISKIMAGE_UNIT  *pDrive=&Drives[DriveID];

         ExtDskInternal_Free(pDrive);

        /* initialise drive */
        memset(pDrive,0,sizeof(DISKIMAGE_UNIT));

		FDD_InsertDisk(DriveID,FALSE);
}

/**********************************************************************/
/* DISK IMAGE ACCESS FUNCTIONS - INTERFACE BETWEEN DISK IMAGE AND FDC */

int             DiskImage_GetSectorsPerTrack(int DriveID, int PhysicalTrack,int PhysicalSide)
{
        DISKIMAGE_UNIT  *pDrive=&Drives[DriveID];

		return ExtDskInternal_GetSectorsPerTrack(pDrive, PhysicalTrack, PhysicalSide);
}

/* get a ID from physical track, physical side and fill in CHRN structure with details */
void    DiskImage_GetID(int DriveID, int PhysicalTrack,int PhysicalSide, int Index, CHRN *pCHRN)
{
        DISKIMAGE_UNIT  *pDrive=&Drives[DriveID];
       
		ExtDskInternal_GetID(pDrive, PhysicalTrack, PhysicalSide, Index, pCHRN);
}

/* get a sector of data from the disk image */
void    DiskImage_GetSector(int DriveID, int PhysicalTrack, int PhysicalSide, int Index, char *pData)
{
        DISKIMAGE_UNIT  *pDrive = &Drives[DriveID];

        ExtDskInternal_GetSector(pDrive, PhysicalTrack, PhysicalSide, Index, pData);
}

/* write a sector of data to the disk image */
void    DiskImage_PutSector(int DriveID, int PhysicalTrack, int PhysicalSide, int Index, char *pData, int Mark)
{
        DISKIMAGE_UNIT *pDrive = &Drives[DriveID];

        pDrive->Flags |= DISKIMAGE_DISK_DIRTY;

	    ExtDskInternal_PutSector(pDrive, PhysicalTrack, PhysicalSide, Index, pData,Mark);
}


/* write a sector of data to the disk image */
void    DiskImage_AddSector(int DriveID, int PhysicalTrack, int PhysicalSide, CHRN *pCHRN, int FormatN,int FillerByte)
{
        DISKIMAGE_UNIT *pDrive = &Drives[DriveID];

        pDrive->Flags |= DISKIMAGE_DISK_DIRTY;

        ExtDskInternal_AddSector(pDrive, PhysicalTrack, PhysicalSide, pCHRN, FormatN,FillerByte);
}

void	DiskImage_EmptyTrack(int DriveID, int PhysicalTrack, int PhysicalSide)
{
    DISKIMAGE_UNIT *pDrive = &Drives[DriveID];

    pDrive->Flags |= DISKIMAGE_DISK_DIRTY;

    ExtDskInternal_EmptyTrack(pDrive, PhysicalTrack, PhysicalSide);
}
	

BOOL    DiskImage_IsImageDirty(int DriveID)
{
    DISKIMAGE_UNIT  *pDrive = &Drives[DriveID];

    if (pDrive->Flags & DISKIMAGE_DISK_DIRTY)
    {
            return TRUE;
    }

    return FALSE;
}

unsigned long DiskImage_CalculateOutputSize(int DriveID)
{
	DISKIMAGE_UNIT  *pDrive = &Drives[DriveID];

	return ExtDskInternal_CalculateOutputDataSize(pDrive);
}


void    DiskImage_GenerateOutputData(unsigned char *pBuffer, int DriveID)
{
       DISKIMAGE_UNIT  *pDrive = &Drives[DriveID];
		
	   ExtDskInternal_GenerateOutputData(pBuffer, pDrive);
}


/* Amiga port (disk write-back, 22 Oct 2026): dskwrite.c reads the drive's
 * internal image and clears its dirty flag */
BOOL	DiskImage_GetUnitPointer(int DriveID, DISKIMAGE_UNIT **ppUnit)
{
	if (DriveID < 0 || DriveID > 3) return FALSE;
	*ppUnit = &Drives[DriveID];
	return TRUE;
}

#ifndef NO_SAVESTATE
/* Amiga port (save states, 28 Oct 2026): a drive's whole disk for cpc/savestate.c,
 * in the core's own form: for each of the 170 track slots its sector IDs (with
 * the stored length) and the bytes the controller can read (the ID's size).
 * The same bytes give the CRC-32 that identifies a disk in a state. On load a
 * new image is built first and only then swapped in. */
#include "../savestate.h"
void SS_DiskImage(SS_IO *io, int drive)
{
	DISKIMAGE_UNIT *u = &Drives[drive];
	EXTDSK_INTERNAL *x = io->load ? ExtDskInternal_New() : (EXTDSK_INTERNAL *)u->pDiskImage;
	int t, s;
	if (io->load && (!x || !x->pTrackList)) { SS_Fail(io, "Not enough memory for the state's disk"); return; }
	for (t = 0; t < 85 * 2 && !io->err; t++) {
		EXTDSK_INTERNAL_TRACK *tr = (x && x->pTrackList) ? x->pTrackList[t] : NULL;
		long n;
		if (!SS_Long(io, tr != NULL)) continue;
		if (io->load) {
			ExtDskInternal_AddTrack(x, t);
			tr = x->pTrackList[t];
			if (!tr) { SS_Fail(io, "Not enough memory for the state's disk"); break; }
		}
		SS_V(io, tr->ModifiedFlag);
		n = SS_Long(io, tr->NoOfSectors);
		if (n < 0 || n > 64) { SS_Fail(io, "The state is damaged (disk track)"); break; }
		for (s = 0; s < n && !io->err; s++) {
			EXTDSKCHRN c;
			long data, size;
			if (!io->load) c = tr->SectorIDs[s];
			SS_Raw(io, &c, sizeof c);
			data = SS_Long(io, io->load ? 0 : tr->pSectorData[s] != NULL);
			size = (c.SectorSizeLow & 0x0ff) | ((c.SectorSizeHigh & 0x0ff) << 8);
			if (io->err) break;
			if (io->load) {
				if (data) {
					int before = tr->NoOfSectors;
					ExtDskInternal_AddSectorToTrack(x, t, &c, 0, size > 0 ? size : 1);
					if (tr->NoOfSectors != before + 1) { SS_Fail(io, "The state's disk could not be built"); break; }
				} else {
					if (tr->NoOfSectors >= 64) { SS_Fail(io, "The state is damaged (disk track)"); break; }
					tr->pSectorData[tr->NoOfSectors++] = NULL;
				}
				tr->SectorIDs[s] = c;
			}
			if (data && size > 0) SS_Pack(io, tr->pSectorData[s], size);
		}
	}
	if (io->load) {
		DISKIMAGE_UNIT tmp;
		tmp.pDiskImage = (unsigned char *)x;
		if (io->err) { ExtDskInternal_Free(&tmp); return; }
		ExtDskInternal_Free(u);
		u->pDiskImage = (unsigned char *)x;
	}
}
/* the unit's flags (inserted, dirty) and image type: read, or set when set != 0 */
int SS_DiskImageFlags(int drive, int *type, int set, int flags, int ntype)
{
	DISKIMAGE_UNIT *u = &Drives[drive];
	if (set) { u->Flags = flags; u->nImageType = ntype; if (!(flags & DISKIMAGE_DISK_INSERTED)) DiskImage_RemoveDisk(drive); }
	if (type) *type = u->nImageType;
	return u->Flags;
}
#endif
