#ifndef COLIBRI_FB_DEV_H
#define COLIBRI_FB_DEV_H

/* XXX: move to common header file, e.g. colibri_dev.h */
struct col_fb_alloc {
	size_t size;		/* in */
	off_t offset;		/* out, suitable for mmap */
	void *vaddr;		/* kernel virtual address */
};

struct col_mmap_to_phys {
	void *user_va;
	void *phys;
};

struct col_capinfo {
	size_t bytesused;	/* out, transferred bytes */
};

#define COL_IOC_FB_ALLOC	_IOR('C', 0, struct col_fb_alloc)
#define COL_IOC_MMAP_TO_PHYS	_IOR('C', 1, struct col_mmap_to_phys)
#define COL_IOC_CAPTURE		_IOR('C', 2, void *)
#define COL_IOC_CAPINFO		_IOR('C', 3, struct col_capinfo)

#endif /* COLIBRI_FB_DEV_H */

/* vim: set sw=8 ts=8 noexpandtab: */
