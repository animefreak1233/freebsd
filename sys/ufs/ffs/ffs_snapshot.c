/*
 * Copyright 2000 Marshall Kirk McKusick. All Rights Reserved.
 *
 * Further information about snapshots can be obtained from:
 *
 *	Marshall Kirk McKusick		http://www.mckusick.com/softdep/
 *	1614 Oxford Street		mckusick@mckusick.com
 *	Berkeley, CA 94709-1608		+1-510-843-9542
 *	USA
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY MARSHALL KIRK MCKUSICK ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL MARSHALL KIRK MCKUSICK BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ffs_snapshot.c	8.11 (McKusick) 7/23/00
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/vnode.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

#define KERNCRED proc0.p_ucred
#define DEBUG 1

static int expunge __P((struct vnode *, struct inode *, struct fs *,
    int (*) __P((struct vnode *, ufs_daddr_t *, ufs_daddr_t *, struct fs *,
    ufs_daddr_t))));
static int indiracct __P((struct vnode *, struct vnode *, int, ufs_daddr_t,
    int, int, int, int, struct fs *, int (*) __P((struct vnode *,
    ufs_daddr_t *, ufs_daddr_t *, struct fs *, ufs_daddr_t))));
static int snapacct __P((struct vnode *, ufs_daddr_t *, ufs_daddr_t *,
    struct fs *, ufs_daddr_t));
static int mapacct __P((struct vnode *, ufs_daddr_t *, ufs_daddr_t *,
    struct fs *, ufs_daddr_t));
static int ffs_copyonwrite __P((struct vnode *, struct buf *));
static int readblock __P((struct buf *, daddr_t));

/*
 * To ensure the consistency of snapshots across crashes, we must
 * synchronously write out copied blocks before allowing the
 * originals to be modified. Because of the rather severe speed
 * penalty that this imposes, the following flag allows this
 * crash persistence to be disabled.
 */
int dopersistence = 0;

#ifdef DEBUG
#include <sys/sysctl.h>
SYSCTL_INT(_debug, OID_AUTO, dopersistence, CTLFLAG_RW, &dopersistence, 0, "");
int snapdebug = 0;
SYSCTL_INT(_debug, OID_AUTO, snapdebug, CTLFLAG_RW, &snapdebug, 0, "");
#endif /* DEBUG */

/*
 * Create a snapshot file and initialize it for the filesystem.
 */
int
ffs_snapshot(mp, snapfile)
	struct mount *mp;
	char *snapfile;
{
	ufs_daddr_t blkno, inoblks[FSMAXSNAP];
	int error, cg, snaploc, indiroff, numblks;
	int i, size, base, len, loc, inoblkcnt;
	int flag = mp->mnt_flag;
	int32_t *lp;
	void *space;
	struct fs *copy_fs = NULL, *fs = VFSTOUFS(mp)->um_fs;
	struct snaphead *snaphead;
	struct thread *td = curthread;
	struct inode *ip, *xp;
	struct buf *bp, *nbp, *ibp, *sbp = NULL;
	struct nameidata nd;
	struct mount *wrtmp;
	struct vattr vat;
	struct vnode *vp;
	struct cg *cgp;

	/*
	 * Need to serialize access to snapshot code per filesystem.
	 */
	/*
	 * Assign a snapshot slot in the superblock.
	 */
	for (snaploc = 0; snaploc < FSMAXSNAP; snaploc++)
		if (fs->fs_snapinum[snaploc] == 0)
			break;
	if (snaploc == FSMAXSNAP)
		return (ENOSPC);
	/*
	 * Create the snapshot file.
	 */
restart:
	NDINIT(&nd, CREATE, LOCKPARENT | LOCKLEAF, UIO_USERSPACE, snapfile, td);
	if ((error = namei(&nd)) != 0)
		return (error);
	if (nd.ni_vp != NULL) {
		vput(nd.ni_vp);
		error = EEXIST;
	}
	if (nd.ni_dvp->v_mount != mp)
		error = EXDEV;
	if (error) {
		NDFREE(&nd, NDF_ONLY_PNBUF);
		if (nd.ni_dvp == nd.ni_vp)
			vrele(nd.ni_dvp);
		else
			vput(nd.ni_dvp);
		return (error);
	}
	VATTR_NULL(&vat);
	vat.va_type = VREG;
	vat.va_mode = S_IRUSR;
	vat.va_vaflags |= VA_EXCLUSIVE;
	if (VOP_GETWRITEMOUNT(nd.ni_dvp, &wrtmp))
		wrtmp = NULL;
	if (wrtmp != mp)
		panic("ffs_snapshot: mount mismatch");
	if (vn_start_write(NULL, &wrtmp, V_NOWAIT) != 0) {
		NDFREE(&nd, NDF_ONLY_PNBUF);
		vput(nd.ni_dvp);
		if ((error = vn_start_write(NULL, &wrtmp,
		    V_XSLEEP | PCATCH)) != 0)
			return (error);
		goto restart;
	}
	VOP_LEASE(nd.ni_dvp, td, KERNCRED, LEASE_WRITE);
	error = VOP_CREATE(nd.ni_dvp, &nd.ni_vp, &nd.ni_cnd, &vat);
	vput(nd.ni_dvp);
	if (error) {
		NDFREE(&nd, NDF_ONLY_PNBUF);
		vn_finished_write(wrtmp);
		return (error);
	}
	vp = nd.ni_vp;
	ip = VTOI(vp);
	/*
	 * Allocate and copy the last block contents so as to be able
	 * to set size to that of the filesystem.
	 */
	numblks = howmany(fs->fs_size, fs->fs_frag);
	error = UFS_BALLOC(vp, lblktosize(fs, (off_t)(numblks - 1)),
	    fs->fs_bsize, KERNCRED, B_CLRBUF, &bp);
	if (error)
		goto out;
	ip->i_size = lblktosize(fs, (off_t)numblks);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	if ((error = readblock(bp, numblks - 1)) != 0)
		goto out;
	bawrite(bp);
	/*
	 * Preallocate critical data structures so that we can copy
	 * them in without further allocation after we suspend all
	 * operations on the filesystem. We would like to just release
	 * the allocated buffers without writing them since they will
	 * be filled in below once we are ready to go, but this upsets
	 * the soft update code, so we go ahead and write the new buffers.
	 *
	 * Allocate all indirect blocks and mark all of them as not
	 * needing to be copied.
	 */
	for (blkno = NDADDR; blkno < numblks; blkno += NINDIR(fs)) {
		error = UFS_BALLOC(vp, lblktosize(fs, (off_t)blkno),
		    fs->fs_bsize, td->td_proc->p_ucred, B_METAONLY, &ibp);
		if (error)
			goto out;
		bdwrite(ibp);
	}
	/*
	 * Allocate shadow blocks to copy all of the other snapshot inodes
	 * so that we will be able to expunge them from this snapshot. Also
	 * include a copy of ourselves so that we do not deadlock trying
	 * to copyonwrite ourselves when VOP_FSYNC'ing below.
	 */
	fs->fs_snapinum[snaploc] = ip->i_number;
	for (loc = snaploc, inoblkcnt = 0; loc >= 0; loc--) {
		blkno = fragstoblks(fs, ino_to_fsba(fs, fs->fs_snapinum[loc]));
		fs->fs_snapinum[snaploc] = 0;
		for (i = 0; i < inoblkcnt; i++)
			if (inoblks[i] == blkno)
				break;
		if (i == inoblkcnt) {
			inoblks[inoblkcnt++] = blkno;
			error = UFS_BALLOC(vp, lblktosize(fs, (off_t)blkno),
			    fs->fs_bsize, KERNCRED, 0, &nbp);
			if (error)
				goto out;
			bawrite(nbp);
		}
	}
	/*
	 * Allocate all cylinder group blocks.
	 */
	for (cg = 0; cg < fs->fs_ncg; cg++) {
		error = UFS_BALLOC(vp, (off_t)(cgtod(fs, cg)) << fs->fs_fshift,
		    fs->fs_bsize, KERNCRED, 0, &nbp);
		if (error)
			goto out;
		bawrite(nbp);
	}
	/*
	 * Allocate copies for the superblock and its summary information.
	 */
	error = UFS_BALLOC(vp, (off_t)(SBOFF), SBSIZE, KERNCRED, 0, &nbp);
	if (error)
		goto out;
	bawrite(nbp);
	blkno = fragstoblks(fs, fs->fs_csaddr);
	len = howmany(fs->fs_cssize, fs->fs_bsize);
	for (loc = 0; loc < len; loc++) {
		error = UFS_BALLOC(vp, lblktosize(fs, (off_t)(blkno + loc)),
		    fs->fs_bsize, KERNCRED, 0, &nbp);
		if (error)
			goto out;
		bawrite(nbp);
	}
	/*
	 * Change inode to snapshot type file.
	 */
	ip->i_flags |= SF_SNAPSHOT;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	/*
	 * Ensure that the snapshot is completely on disk.
	 */
	if ((error = VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td)) != 0)
		goto out;
	/*
	 * All allocations are done, so we can now snapshot the system.
	 *
	 * Suspend operation on filesystem.
	 */
	for (;;) {
		vn_finished_write(wrtmp);
		vfs_write_suspend(vp->v_mount);
		if (mp->mnt_kern_flag & MNTK_SUSPENDED)
			break;
		vn_start_write(NULL, &wrtmp, V_WAIT);
	}
	/*
	 * First, copy all the cylinder group maps. All the unallocated
	 * blocks are marked BLK_NOCOPY so that the snapshot knows that
	 * it need not copy them if they are later written.
	 */
	len = howmany(fs->fs_fpg, fs->fs_frag);
	for (cg = 0; cg < fs->fs_ncg; cg++) {
		error = bread(ip->i_devvp, fsbtodb(fs, cgtod(fs, cg)),
			(int)fs->fs_cgsize, KERNCRED, &bp);
		if (error) {
			brelse(bp);
			goto out1;
		}
		cgp = (struct cg *)bp->b_data;
		if (!cg_chkmagic(cgp)) {
			brelse(bp);
			error = EIO;
			goto out1;
		}
		error = bread(vp, fragstoblks(fs, cgtod(fs, cg)), fs->fs_bsize,
			KERNCRED, &nbp);
		if (error) {
			brelse(bp);
			brelse(nbp);
			goto out1;
		}
		bcopy(bp->b_data, nbp->b_data, fs->fs_cgsize);
		if (fs->fs_cgsize < fs->fs_bsize)
			bzero(&nbp->b_data[fs->fs_cgsize],
			    fs->fs_bsize - fs->fs_cgsize);
		nbp->b_flags |= B_VALIDSUSPWRT;
		bawrite(nbp);
		base = cg * fs->fs_fpg / fs->fs_frag;
		if (base + len >= numblks)
			len = numblks - base - 1;
		loc = 0;
		if (base < NDADDR) {
			for ( ; loc < NDADDR; loc++) {
				if (!ffs_isblock(fs, cg_blksfree(cgp), loc))
					continue;
				ip->i_db[loc] = BLK_NOCOPY;
			}
		}
		error = UFS_BALLOC(vp, lblktosize(fs, (off_t)(base + loc)),
		    fs->fs_bsize, KERNCRED, B_METAONLY, &ibp);
		if (error) {
			brelse(bp);
			goto out1;
		}
		indiroff = (base + loc - NDADDR) % NINDIR(fs);
		for ( ; loc < len; loc++, indiroff++) {
			if (indiroff >= NINDIR(fs)) {
				ibp->b_flags |= B_VALIDSUSPWRT;
				bawrite(ibp);
				error = UFS_BALLOC(vp,
				    lblktosize(fs, (off_t)(base + loc)),
				    fs->fs_bsize, KERNCRED, B_METAONLY, &ibp);
				if (error) {
					brelse(bp);
					goto out1;
				}
				indiroff = 0;
			}
			if (!ffs_isblock(fs, cg_blksfree(cgp), loc))
				continue;
			if (((ufs_daddr_t *)(ibp->b_data))[indiroff] != 0)
				panic("ffs_snapshot: lost block");
			((ufs_daddr_t *)(ibp->b_data))[indiroff] = BLK_NOCOPY;
		}
		bqrelse(bp);
		ibp->b_flags |= B_VALIDSUSPWRT;
		bdwrite(ibp);
	}
	/*
	 * Copy the shadow blocks for the snapshot inodes so that
	 * the copies can can be expunged.
	 */
	for (loc = 0; loc < inoblkcnt; loc++) {
		error = bread(vp, inoblks[loc], fs->fs_bsize, KERNCRED, &nbp);
		if (error)
			goto out1;
		readblock(nbp, inoblks[loc]);
		nbp->b_flags |= B_VALIDSUSPWRT;
		bdwrite(nbp);
	}
	/*
	 * Copy allocation information from all the snapshots in
	 * this snapshot and then expunge them from its view.
	 */
	snaphead = &ip->i_devvp->v_rdev->si_snapshots;
	TAILQ_FOREACH(xp, snaphead, i_nextsnap)
		if ((error = expunge(vp, xp, fs, snapacct)) != 0)
			goto out1;
	/*
	 * Grab a copy of the superblock and its summary information.
	 * We delay writing it until the suspension is released below.
	 */
	error = bread(vp, lblkno(fs, SBOFF), fs->fs_bsize, KERNCRED, &sbp);
	if (error)
		goto out1;
	copy_fs = (struct fs *)(sbp->b_data + blkoff(fs, SBOFF));
	bcopy(fs, copy_fs, fs->fs_sbsize);
	if ((fs->fs_flags & (FS_UNCLEAN | FS_NEEDSFSCK)) == 0)
		copy_fs->fs_clean = 1;
	if (fs->fs_sbsize < SBSIZE)
		bzero(&sbp->b_data[blkoff(fs, SBOFF) + fs->fs_sbsize],
		    SBSIZE - fs->fs_sbsize);
	size = blkroundup(fs, fs->fs_cssize);
	if (fs->fs_contigsumsize > 0)
		size += fs->fs_ncg * sizeof(int32_t);
	space = malloc((u_long)size, M_UFSMNT, M_WAITOK);
	copy_fs->fs_csp = space;
	bcopy(fs->fs_csp, copy_fs->fs_csp, fs->fs_cssize);
	(char *)space += fs->fs_cssize;
	loc = howmany(fs->fs_cssize, fs->fs_fsize);
	i = fs->fs_frag - loc % fs->fs_frag;
	len = (i == fs->fs_frag) ? 0 : i * fs->fs_fsize;
	if (len > 0) {
		if ((error = bread(ip->i_devvp,
		    fsbtodb(fs, fs->fs_csaddr + loc),
		    len, KERNCRED, &bp)) != 0) {
			free(copy_fs->fs_csp, M_UFSMNT);
			goto out1;
		}
		bcopy(bp->b_data, space, (u_int)len);
		(char *)space += len;
		bp->b_flags |= B_INVAL | B_NOCACHE;
		brelse(bp);
	}
	if (fs->fs_contigsumsize > 0) {
		copy_fs->fs_maxcluster = lp = space;
		for (i = 0; i < fs->fs_ncg; i++)
			*lp++ = fs->fs_contigsumsize;
	}
	/*
	 * Record snapshot inode. Since this is the newest snapshot,
	 * it must be placed at the end of the list.
	 */
	fs->fs_snapinum[snaploc] = ip->i_number;
	if (ip->i_nextsnap.tqe_prev != 0)
		panic("ffs_snapshot: %d already on list", ip->i_number);
	TAILQ_INSERT_TAIL(snaphead, ip, i_nextsnap);
	ip->i_devvp->v_rdev->si_copyonwrite = ffs_copyonwrite;
	ip->i_devvp->v_flag |= VCOPYONWRITE;
	vp->v_flag |= VSYSTEM;
	/*
	 * Resume operation on filesystem.
	 */
out1:
	vfs_write_resume(vp->v_mount);
	vn_start_write(NULL, &wrtmp, V_WAIT);
	if (sbp != NULL) {
		/*
		 * Expunge the blocks used by the snapshots from the set of
		 * blocks marked as used in the snapshot bitmaps.
		 */
		if ((error = expunge(vp, VTOI(vp), copy_fs, mapacct)) != 0) {
			vref(vp);
			ffs_snapgone(VTOI(vp));
			free(copy_fs->fs_csp, M_UFSMNT);
			bawrite(sbp);
			goto out;
		}
		/*
		 * Write the superblock and its summary information
		 * to the snapshot.
		 */
		blkno = fragstoblks(fs, fs->fs_csaddr);
		len = howmany(fs->fs_cssize, fs->fs_bsize);
		space = copy_fs->fs_csp;
		for (loc = 0; loc < len; loc++) {
			error = bread(vp, blkno + loc, fs->fs_bsize,
				KERNCRED, &nbp);
			if (error) {
				vref(vp);
				ffs_snapgone(VTOI(vp));
				free(copy_fs->fs_csp, M_UFSMNT);
				bawrite(sbp);
				goto out;
			}
			bcopy(space, nbp->b_data, fs->fs_bsize);
			space = (char *)space + fs->fs_bsize;
			bawrite(nbp);
		}
		free(copy_fs->fs_csp, M_UFSMNT);
		bawrite(sbp);
	}
out:
	mp->mnt_flag = flag;
	if (error)
		(void) UFS_TRUNCATE(vp, (off_t)0, 0, NOCRED, td);
	(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
	if (error)
		vput(vp);
	else
		VOP_UNLOCK(vp, 0, td);
	vn_finished_write(wrtmp);
	return (error);
}

/*
 * Before expunging a snapshot inode, note all the
 * blocks that it claims with BLK_SNAP so that fsck will
 * be able to account for those blocks properly and so
 * that this snapshot knows that it need not copy them
 * if the other snapshot holding them is freed.
 */
static int
expunge(vp, xp, fs, acctfunc)
	struct vnode *vp;
	struct inode *xp;
	struct fs *fs;
	int (*acctfunc) __P((struct vnode *, ufs_daddr_t *, ufs_daddr_t *,
	    struct fs *, ufs_daddr_t));
{
	int i, len, error, numblks, blksperindir;
	ufs_daddr_t lbn, rlbn, blkno;
	struct dinode *dip;
	struct buf *bp;

	if ((error = (*acctfunc)(vp, &xp->i_db[0], &xp->i_ib[NIADDR], fs, 0)))
		return (error);
	numblks = howmany(fs->fs_size, fs->fs_frag);
	blksperindir = 1;
	lbn = -NDADDR;
	len = numblks - NDADDR;
	rlbn = NDADDR;
	for (i = 0; len > 0 && i < NIADDR; i++) {
		error = indiracct(vp, ITOV(xp), i, xp->i_ib[i], lbn,
		    rlbn, len, blksperindir, fs, acctfunc);
		if (error)
			return (error);
		blksperindir *= NINDIR(fs);
		lbn -= blksperindir + 1;
		len -= blksperindir;
		rlbn += blksperindir;
	}
	/*
	 * Set copied snapshot inode to be a zero length file.
	 */
	blkno = fragstoblks(fs, ino_to_fsba(fs, xp->i_number));
	if ((error = bread(vp, blkno, fs->fs_bsize, KERNCRED, &bp)) != 0)
		return (error);
	dip = (struct dinode *)bp->b_data +
	    ino_to_fsbo(fs, xp->i_number);
	dip->di_size = 0;
	dip->di_blocks = 0;
	dip->di_flags &= ~SF_SNAPSHOT;
	bzero(&dip->di_db[0], (NDADDR + NIADDR) * sizeof(ufs_daddr_t));
	bp->b_flags |= B_VALIDSUSPWRT;
	bdwrite(bp);
	return (0);
}

/*
 * Descend an indirect block chain for vnode cancelvp accounting for all
 * its indirect blocks in snapvp.
 */ 
static int
indiracct(snapvp, cancelvp, level, blkno, lbn, rlbn, remblks, blksperindir, fs,
	    acctfunc)
	struct vnode *snapvp;
	struct vnode *cancelvp;
	int level;
	ufs_daddr_t blkno;
	int lbn;
	int rlbn;
	int remblks;
	int blksperindir;
	struct fs *fs;
	int (*acctfunc) __P((struct vnode *, ufs_daddr_t *, ufs_daddr_t *,
	    struct fs *, ufs_daddr_t));
{
	int subblksperindir, error, last, num, i;
	struct indir indirs[NIADDR + 2];
	ufs_daddr_t *bap;
	struct buf *bp;

	if ((error = ufs_getlbns(cancelvp, rlbn, indirs, &num)) != 0)
		return (error);
	if (lbn != indirs[num - 1 - level].in_lbn || blkno == 0 || num < 2)
		panic("indiracct: botched params");
	/*
	 * We have to expand bread here since it will deadlock looking
	 * up the block number for any blocks that are not in the cache.
	 */
	bp = getblk(cancelvp, lbn, fs->fs_bsize, 0, 0);
	bp->b_blkno = fsbtodb(fs, blkno);
	if ((bp->b_flags & (B_DONE | B_DELWRI)) == 0 &&
	    (error = readblock(bp, fragstoblks(fs, blkno)))) {
		brelse(bp);
		return (error);
	}
	/*
	 * Account for the block pointers in this indirect block.
	 */
	last = howmany(remblks, blksperindir);
	if (last > NINDIR(fs))
		last = NINDIR(fs);
	MALLOC(bap, ufs_daddr_t *, fs->fs_bsize, M_DEVBUF, M_WAITOK);
	bcopy(bp->b_data, (caddr_t)bap, fs->fs_bsize);
	bqrelse(bp);
	error = (*acctfunc)(snapvp, &bap[0], &bap[last], fs, rlbn);
	if (error || level == 0)
		goto out;
	/*
	 * Account for the block pointers in each of the indirect blocks
	 * in the levels below us.
	 */
	subblksperindir = blksperindir / NINDIR(fs);
	for (lbn++, level--, i = 0; i < last; i++) {
		error = indiracct(snapvp, cancelvp, level, bap[i], lbn,
		    rlbn, remblks, subblksperindir, fs, acctfunc);
		if (error)
			goto out;
		rlbn += blksperindir;
		lbn -= blksperindir;
		remblks -= blksperindir;
	}
out:
	FREE(bap, M_DEVBUF);
	return (error);
}

/*
 * Account for a set of blocks allocated in a snapshot inode.
 */
static int
snapacct(vp, oldblkp, lastblkp, fs, lblkno)
	struct vnode *vp;
	ufs_daddr_t *oldblkp, *lastblkp;
	struct fs *fs;
	ufs_daddr_t lblkno;
{
	struct inode *ip = VTOI(vp);
	ufs_daddr_t lbn, blkno, *blkp;
	struct buf *ibp;
	int error;

	for ( ; oldblkp < lastblkp; oldblkp++) {
		blkno = *oldblkp;
		if (blkno == 0 || blkno == BLK_NOCOPY || blkno == BLK_SNAP)
			continue;
		lbn = fragstoblks(fs, blkno);
		if (lbn < NDADDR) {
			blkp = &ip->i_db[lbn];
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
		} else {
			error = UFS_BALLOC(vp, lblktosize(fs, (off_t)lbn),
			    fs->fs_bsize, KERNCRED, B_METAONLY, &ibp);
			if (error)
				return (error);
			blkp = &((ufs_daddr_t *)(ibp->b_data))
			    [(lbn - NDADDR) % NINDIR(fs)];
		}
		if (*blkp != 0)
			panic("snapacct: bad block");
		*blkp = BLK_SNAP;
		if (lbn >= NDADDR) {
			ibp->b_flags |= B_VALIDSUSPWRT;
			bdwrite(ibp);
		}
	}
	return (0);
}

/*
 * Account for a set of blocks allocated in a snapshot inode.
 */
static int
mapacct(vp, oldblkp, lastblkp, fs, lblkno)
	struct vnode *vp;
	ufs_daddr_t *oldblkp, *lastblkp;
	struct fs *fs;
	ufs_daddr_t lblkno;
{
	struct inode *ip = VTOI(vp);
	ufs_daddr_t blkno, cgblkno, fragno;
	struct buf *bp;
	struct cg *cgp;
	char *blksfree;
	int i, cg, error;

	for ( ; oldblkp < lastblkp; oldblkp++, lblkno++) {
		blkno = *oldblkp;
		if (blkno == 0 || blkno == BLK_NOCOPY)
			continue;
		if (blkno == BLK_SNAP)
			blkno = blkstofrags(fs, lblkno);
		cg = dtog(fs, blkno);
		cgblkno = fragstoblks(fs, cgtod(fs, cg));
		if ((error = bread(vp, cgblkno, fs->fs_bsize, KERNCRED, &bp)))
			return (error);
		cgp = (struct cg *)bp->b_data;
		if (!cg_chkmagic(cgp) || cgp->cg_cgx != cg) {
			if (!cg_chkmagic(cgp))
				printf("mapacct: bad magic 0x%x\n",
				    cgp->cg_magic);
			else
				printf("%s: mismatched cg %d != cg_cgx %d\n",
				    "mapacct", cg, cgp->cg_cgx);
			brelse(bp);
			return (EIO);
		}
		cgp->cg_time = time_second;
		cgblkno = dtogd(fs, blkno);
		blksfree = cg_blksfree(cgp);
		fragno = fragstoblks(fs, cgblkno);
		if (!ffs_isfreeblock(fs, blksfree, fragno)) {
			printf("dev = %s, block = %ld, fs = %s\n",
			    devtoname(ip->i_dev), (long)blkno, fs->fs_fsmnt);
			panic("mapacct: freeing free block");
		}
		ffs_setblock(fs, blksfree, fragno);
		ffs_clusteracct(fs, cgp, fragno, 1);
		cgp->cg_cs.cs_nbfree++;
		fs->fs_cstotal.cs_nbfree++;
		fs->fs_cs(fs, cg).cs_nbfree++;
		i = cbtocylno(fs, cgblkno);
		cg_blks(fs, cgp, i)[cbtorpos(fs, cgblkno)]++;
		cg_blktot(cgp)[i]++;
		fs->fs_fmod = 1;
		bdwrite(bp);
	}
	return (0);
}

/*
 * Decrement extra reference on snapshot when last name is removed.
 * It will not be freed until the last open reference goes away.
 */
void
ffs_snapgone(ip)
	struct inode *ip;
{
	struct inode *xp;
	struct fs *fs;
	int snaploc;

	/*
	 * Find snapshot in incore list.
	 */
	TAILQ_FOREACH(xp, &ip->i_devvp->v_rdev->si_snapshots, i_nextsnap)
		if (xp == ip)
			break;
	if (xp == 0)
		printf("ffs_snapgone: lost snapshot vnode %d\n",
		    ip->i_number);
	else
		vrele(ITOV(ip));
	/*
	 * Delete snapshot inode from superblock. Keep list dense.
	 */
	fs = ip->i_fs;
	for (snaploc = 0; snaploc < FSMAXSNAP; snaploc++)
		if (fs->fs_snapinum[snaploc] == ip->i_number)
			break;
	if (snaploc < FSMAXSNAP) {
		for (snaploc++; snaploc < FSMAXSNAP; snaploc++) {
			if (fs->fs_snapinum[snaploc] == 0)
				break;
			fs->fs_snapinum[snaploc - 1] = fs->fs_snapinum[snaploc];
		}
		fs->fs_snapinum[snaploc - 1] = 0;
	}
}

/*
 * Prepare a snapshot file for being removed.
 */
void
ffs_snapremove(vp)
	struct vnode *vp;
{
	struct inode *ip;
	struct vnode *devvp;
	struct buf *ibp;
	struct fs *fs;
	ufs_daddr_t blkno, dblk;
	int error, numblks, loc, last;

	ip = VTOI(vp);
	fs = ip->i_fs;
	/*
	 * If active, delete from incore list (this snapshot may
	 * already have been in the process of being deleted, so
	 * would not have been active).
	 *
	 * Clear copy-on-write flag if last snapshot.
	 */
	if (ip->i_nextsnap.tqe_prev != 0) {
		devvp = ip->i_devvp;
		TAILQ_REMOVE(&devvp->v_rdev->si_snapshots, ip, i_nextsnap);
		ip->i_nextsnap.tqe_prev = 0;
		if (TAILQ_FIRST(&devvp->v_rdev->si_snapshots) == 0) {
			devvp->v_rdev->si_copyonwrite = 0;
			devvp->v_flag &= ~VCOPYONWRITE;
		}
	}
	/*
	 * Clear all BLK_NOCOPY fields. Pass any block claims to other
	 * snapshots that want them (see ffs_snapblkfree below).
	 */
	for (blkno = 1; blkno < NDADDR; blkno++) {
		dblk = ip->i_db[blkno];
		if (dblk == BLK_NOCOPY || dblk == BLK_SNAP)
			ip->i_db[blkno] = 0;
		else if ((dblk == blkstofrags(fs, blkno) &&
		     ffs_snapblkfree(ip, dblk, fs->fs_bsize))) {
			ip->i_blocks -= btodb(fs->fs_bsize);
			ip->i_db[blkno] = 0;
		}
	}
	numblks = howmany(ip->i_size, fs->fs_bsize);
	for (blkno = NDADDR; blkno < numblks; blkno += NINDIR(fs)) {
		error = UFS_BALLOC(vp, lblktosize(fs, (off_t)blkno),
		    fs->fs_bsize, KERNCRED, B_METAONLY, &ibp);
		if (error)
			continue;
		if ((last = fs->fs_size - blkno) > NINDIR(fs))
			last = NINDIR(fs);
		for (loc = 0; loc < last; loc++) {
			dblk = ((ufs_daddr_t *)(ibp->b_data))[loc];
			if (dblk == BLK_NOCOPY || dblk == BLK_SNAP)
				((ufs_daddr_t *)(ibp->b_data))[loc] = 0;
			else if ((dblk == blkstofrags(fs, blkno) &&
			     ffs_snapblkfree(ip, dblk, fs->fs_bsize))) {
				ip->i_blocks -= btodb(fs->fs_bsize);
				((ufs_daddr_t *)(ibp->b_data))[loc] = 0;
			}
		}
		bawrite(ibp);
	}
	/*
	 * Clear snapshot flag and drop reference.
	 */
	ip->i_flags &= ~SF_SNAPSHOT;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
}

/*
 * Notification that a block is being freed. Return zero if the free
 * should be allowed to proceed. Return non-zero if the snapshot file
 * wants to claim the block. The block will be claimed if it is an
 * uncopied part of one of the snapshots. It will be freed if it is
 * either a BLK_NOCOPY or has already been copied in all of the snapshots.
 * If a fragment is being freed, then all snapshots that care about
 * it must make a copy since a snapshot file can only claim full sized
 * blocks. Note that if more than one snapshot file maps the block,
 * we can pick one at random to claim it. Since none of the snapshots
 * can change, we are assurred that they will all see the same unmodified
 * image. When deleting a snapshot file (see ffs_snapremove above), we
 * must push any of these claimed blocks to one of the other snapshots
 * that maps it. These claimed blocks are easily identified as they will
 * have a block number equal to their logical block number within the
 * snapshot. A copied block can never have this property because they
 * must always have been allocated from a BLK_NOCOPY location.
 */
int
ffs_snapblkfree(freeip, bno, size)
	struct inode *freeip;
	ufs_daddr_t bno;
	long size;
{
	struct buf *ibp, *cbp, *savedcbp = 0;
	struct fs *fs = freeip->i_fs;
	struct thread *td = curthread;
	struct inode *ip;
	struct vnode *vp;
	ufs_daddr_t lbn, blkno;
	int indiroff = 0, error = 0, claimedblk = 0;
	struct snaphead *snaphead;

	lbn = fragstoblks(fs, bno);
	snaphead = &freeip->i_devvp->v_rdev->si_snapshots;
	TAILQ_FOREACH(ip, snaphead, i_nextsnap) {
		vp = ITOV(ip);
		/*
		 * Lookup block being written.
		 */
		if (lbn < NDADDR) {
			blkno = ip->i_db[lbn];
		} else {
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
			td->td_proc->p_flag |= P_COWINPROGRESS;
			error = UFS_BALLOC(vp, lblktosize(fs, (off_t)lbn),
			    fs->fs_bsize, KERNCRED, B_METAONLY, &ibp);
			td->td_proc->p_flag &= ~P_COWINPROGRESS;
			VOP_UNLOCK(vp, 0, td);
			if (error)
				break;
			indiroff = (lbn - NDADDR) % NINDIR(fs);
			blkno = ((ufs_daddr_t *)(ibp->b_data))[indiroff];
		}
		/*
		 * Check to see if block needs to be copied.
		 */
		switch (blkno) {
		/*
		 * If the snapshot has already copied the block (default),
		 * or does not care about the block, it is not needed.
		 */
		default:
		case BLK_NOCOPY:
			if (lbn >= NDADDR)
				bqrelse(ibp);
			continue;
		/*
		 * No previous snapshot claimed the block, so it will be
		 * freed and become a BLK_NOCOPY (don't care) for us.
		 */
		case BLK_SNAP:
			if (claimedblk)
				panic("snapblkfree: inconsistent block type");
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
			if (lbn < NDADDR) {
				ip->i_db[lbn] = BLK_NOCOPY;
				ip->i_flag |= IN_CHANGE | IN_UPDATE;
			} else {
				((ufs_daddr_t *)(ibp->b_data))[indiroff] =
				    BLK_NOCOPY;
				bdwrite(ibp);
			}
			VOP_UNLOCK(vp, 0, td);
			continue;
		/*
		 * A block that we map is being freed. If it has not been
		 * claimed yet, we will claim or copy it (below).
		 */
		case 0:
			claimedblk = 1;
			break;
		}
		/*
		 * If this is a full size block, we will just grab it
		 * and assign it to the snapshot inode. Otherwise we
		 * will proceed to copy it. See explanation for this
		 * routine as to why only a single snapshot needs to
		 * claim this block.
		 */
		if (size == fs->fs_bsize) {
#ifdef DEBUG
			if (snapdebug)
				printf("%s %d lbn %d from inum %d\n",
				    "Grabonremove: snapino", ip->i_number, lbn,
				    freeip->i_number);
#endif
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
			if (lbn < NDADDR) {
				ip->i_db[lbn] = bno;
			} else {
				((ufs_daddr_t *)(ibp->b_data))[indiroff] = bno;
				bdwrite(ibp);
			}
			ip->i_blocks += btodb(size);
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
			VOP_UNLOCK(vp, 0, td);
			return (1);
		}
		if (lbn >= NDADDR)
			bqrelse(ibp);
		/*
		 * Allocate the block into which to do the copy. Note that this
		 * allocation will never require any additional allocations for
		 * the snapshot inode.
		 */
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
		td->td_proc->p_flag |= P_COWINPROGRESS;
		error = UFS_BALLOC(vp, lblktosize(fs, (off_t)lbn),
		    fs->fs_bsize, KERNCRED, 0, &cbp);
		td->td_proc->p_flag &= ~P_COWINPROGRESS;
		if (error) {
			VOP_UNLOCK(vp, 0, td);
			break;
		}
#ifdef DEBUG
		if (snapdebug)
			printf("%s%d lbn %d for inum %d size %ld to blkno %d\n",
			    "Copyonremove: snapino ", ip->i_number, lbn,
			    freeip->i_number, size, cbp->b_blkno);
#endif
		/*
		 * If we have already read the old block contents, then
		 * simply copy them to the new block. Note that we need
		 * to synchronously write snapshots that have not been
		 * unlinked, and hence will be visible after a crash,
		 * to ensure their integrity.
		 */
		if (savedcbp != 0) {
			bcopy(savedcbp->b_data, cbp->b_data, fs->fs_bsize);
			bawrite(cbp);
			if (dopersistence && ip->i_effnlink > 0)
				(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
			VOP_UNLOCK(vp, 0, td);
			continue;
		}
		/*
		 * Otherwise, read the old block contents into the buffer.
		 */
		if ((error = readblock(cbp, lbn)) != 0) {
			bzero(cbp->b_data, fs->fs_bsize);
			bawrite(cbp);
			if (dopersistence && ip->i_effnlink > 0)
				(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
			VOP_UNLOCK(vp, 0, td);
			break;
		}
		VOP_UNLOCK(vp, 0, td);
		savedcbp = cbp;
	}
	/*
	 * Note that we need to synchronously write snapshots that
	 * have not been unlinked, and hence will be visible after
	 * a crash, to ensure their integrity.
	 */
	if (savedcbp) {
		vp = savedcbp->b_vp;
		bawrite(savedcbp);
		if (dopersistence && VTOI(vp)->i_effnlink > 0) {
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
			(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
			VOP_UNLOCK(vp, 0, td);
		}
	}
	/*
	 * If we have been unable to allocate a block in which to do
	 * the copy, then return non-zero so that the fragment will
	 * not be freed. Although space will be lost, the snapshot
	 * will stay consistent.
	 */
	return (error);
}

/*
 * Associate snapshot files when mounting.
 */
void
ffs_snapshot_mount(mp)
	struct mount *mp;
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct fs *fs = ump->um_fs;
	struct thread *td = curthread;
	struct snaphead *snaphead;
	struct vnode *vp;
	struct inode *ip;
	int error, snaploc, loc;

	snaphead = &ump->um_devvp->v_rdev->si_snapshots;
	for (snaploc = 0; snaploc < FSMAXSNAP; snaploc++) {
		if (fs->fs_snapinum[snaploc] == 0)
			return;
		if ((error = VFS_VGET(mp, fs->fs_snapinum[snaploc], &vp)) != 0){
			printf("ffs_snapshot_mount: vget failed %d\n", error);
			continue;
		}
		ip = VTOI(vp);
		if ((ip->i_flags & SF_SNAPSHOT) == 0) {
			printf("ffs_snapshot_mount: non-snapshot inode %d\n",
			    fs->fs_snapinum[snaploc]);
			vput(vp);
			for (loc = snaploc + 1; loc < FSMAXSNAP; loc++) {
				if (fs->fs_snapinum[loc] == 0)
					break;
				fs->fs_snapinum[loc - 1] = fs->fs_snapinum[loc];
			}
			fs->fs_snapinum[loc - 1] = 0;
			snaploc--;
			continue;
		}
		if (ip->i_nextsnap.tqe_prev != 0)
			panic("ffs_snapshot_mount: %d already on list",
			    ip->i_number);
		else
			TAILQ_INSERT_TAIL(snaphead, ip, i_nextsnap);
		vp->v_flag |= VSYSTEM;
		ump->um_devvp->v_rdev->si_copyonwrite = ffs_copyonwrite;
		ump->um_devvp->v_flag |= VCOPYONWRITE;
		VOP_UNLOCK(vp, 0, td);
	}
}

/*
 * Disassociate snapshot files when unmounting.
 */
void
ffs_snapshot_unmount(mp)
	struct mount *mp;
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct snaphead *snaphead = &ump->um_devvp->v_rdev->si_snapshots;
	struct inode *xp;

	while ((xp = TAILQ_FIRST(snaphead)) != 0) {
		TAILQ_REMOVE(snaphead, xp, i_nextsnap);
		xp->i_nextsnap.tqe_prev = 0;
		if (xp->i_effnlink > 0)
			vrele(ITOV(xp));
	}
	ump->um_devvp->v_rdev->si_copyonwrite = 0;
	ump->um_devvp->v_flag &= ~VCOPYONWRITE;
}

/*
 * Check for need to copy block that is about to be written,
 * copying the block if necessary.
 */
static int
ffs_copyonwrite(devvp, bp)
	struct vnode *devvp;
	struct buf *bp;
{
	struct buf *ibp, *cbp, *savedcbp = 0;
	struct thread *td = curthread;
	struct fs *fs;
	struct inode *ip;
	struct vnode *vp;
	ufs_daddr_t lbn, blkno;
	int indiroff, error = 0;

	fs = TAILQ_FIRST(&devvp->v_rdev->si_snapshots)->i_fs;
	lbn = fragstoblks(fs, dbtofsb(fs, bp->b_blkno));
	if (td->td_proc->p_flag & P_COWINPROGRESS)
		panic("ffs_copyonwrite: recursive call");
	TAILQ_FOREACH(ip, &devvp->v_rdev->si_snapshots, i_nextsnap) {
		vp = ITOV(ip);
		/*
		 * We ensure that everything of our own that needs to be
		 * copied will be done at the time that ffs_snapshot is
		 * called. Thus we can skip the check here which can
		 * deadlock in doing the lookup in UFS_BALLOC.
		 */
		if (bp->b_vp == vp)
			continue;
		/*
		 * Check to see if block needs to be copied. We have to
		 * be able to do the UFS_BALLOC without blocking, otherwise
		 * we may get in a deadlock with another process also
		 * trying to allocate. If we find outselves unable to
		 * get the buffer lock, we unlock the snapshot vnode,
		 * sleep briefly, and try again.
		 */
retry:
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
		if (lbn < NDADDR) {
			blkno = ip->i_db[lbn];
		} else {
			td->td_proc->p_flag |= P_COWINPROGRESS;
			error = UFS_BALLOC(vp, lblktosize(fs, (off_t)lbn),
			   fs->fs_bsize, KERNCRED, B_METAONLY | B_NOWAIT, &ibp);
			td->td_proc->p_flag &= ~P_COWINPROGRESS;
			if (error) {
				VOP_UNLOCK(vp, 0, td);
				if (error != EWOULDBLOCK)
					break;
				tsleep(vp, td->td_ksegrp->kg_pri.pri_user, "nap", 1);
				goto retry;
			}
			indiroff = (lbn - NDADDR) % NINDIR(fs);
			blkno = ((ufs_daddr_t *)(ibp->b_data))[indiroff];
			bqrelse(ibp);
		}
#ifdef DIAGNOSTIC
		if (blkno == BLK_SNAP && bp->b_lblkno >= 0)
			panic("ffs_copyonwrite: bad copy block");
#endif
		if (blkno != 0) {
			VOP_UNLOCK(vp, 0, td);
			continue;
		}
		/*
		 * Allocate the block into which to do the copy. Note that this
		 * allocation will never require any additional allocations for
		 * the snapshot inode.
		 */
		td->td_proc->p_flag |= P_COWINPROGRESS;
		error = UFS_BALLOC(vp, lblktosize(fs, (off_t)lbn),
		    fs->fs_bsize, KERNCRED, B_NOWAIT, &cbp);
		td->td_proc->p_flag &= ~P_COWINPROGRESS;
		if (error) {
			VOP_UNLOCK(vp, 0, td);
			if (error != EWOULDBLOCK)
				break;
			tsleep(vp, td->td_ksegrp->kg_pri.pri_user, "nap", 1);
			goto retry;
		}
#ifdef DEBUG
		if (snapdebug) {
			printf("Copyonwrite: snapino %d lbn %d for ",
			    ip->i_number, lbn);
			if (bp->b_vp == devvp)
				printf("fs metadata");
			else
				printf("inum %d", VTOI(bp->b_vp)->i_number);
			printf(" lblkno %d to blkno %d\n", bp->b_lblkno,
			    cbp->b_blkno);
		}
#endif
		/*
		 * If we have already read the old block contents, then
		 * simply copy them to the new block. Note that we need
		 * to synchronously write snapshots that have not been
		 * unlinked, and hence will be visible after a crash,
		 * to ensure their integrity.
		 */
		if (savedcbp != 0) {
			bcopy(savedcbp->b_data, cbp->b_data, fs->fs_bsize);
			bawrite(cbp);
			if (dopersistence && ip->i_effnlink > 0)
				(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
			VOP_UNLOCK(vp, 0, td);
			continue;
		}
		/*
		 * Otherwise, read the old block contents into the buffer.
		 */
		if ((error = readblock(cbp, lbn)) != 0) {
			bzero(cbp->b_data, fs->fs_bsize);
			bawrite(cbp);
			if (dopersistence && ip->i_effnlink > 0)
				(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
			VOP_UNLOCK(vp, 0, td);
			break;
		}
		savedcbp = cbp;
		VOP_UNLOCK(vp, 0, td);
	}
	/*
	 * Note that we need to synchronously write snapshots that
	 * have not been unlinked, and hence will be visible after
	 * a crash, to ensure their integrity.
	 */
	if (savedcbp) {
		vp = savedcbp->b_vp;
		bawrite(savedcbp);
		if (dopersistence && VTOI(vp)->i_effnlink > 0) {
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
			(void) VOP_FSYNC(vp, KERNCRED, MNT_WAIT, td);
			VOP_UNLOCK(vp, 0, td);
		}
	}
	return (error);
}

/*
 * Read the specified block into the given buffer.
 * Much of this boiler-plate comes from bwrite().
 */
static int
readblock(bp, lbn)
	struct buf *bp;
	daddr_t lbn;
{
	struct uio auio;
	struct iovec aiov;
	struct thread *td = curthread;
	struct inode *ip = VTOI(bp->b_vp);

	aiov.iov_base = bp->b_data;
	aiov.iov_len = bp->b_bcount;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = dbtob(fsbtodb(ip->i_fs, blkstofrags(ip->i_fs, lbn)));
	auio.uio_resid = bp->b_bcount;
	auio.uio_rw = UIO_READ;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_td = td;
	return (physio(ip->i_devvp->v_rdev, &auio, 0));
}
