

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/inet.h>
#include <linux/pagemap.h>
#include <linux/idr.h>
#include <linux/sched.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>

#include "v9fs.h"
#include "v9fs_vfs.h"
#include "cache.h"


static int v9fs_vfs_readpage(struct file *filp, struct page *page)
{
	int retval;
	loff_t offset;
	char *buffer;
	struct inode *inode;

	inode = page->mapping->host;
	P9_DPRINTK(P9_DEBUG_VFS, "\n");

	BUG_ON(!PageLocked(page));

	retval = v9fs_readpage_from_fscache(inode, page);
	if (retval == 0)
		return retval;

	buffer = kmap(page);
	offset = page_offset(page);

	retval = v9fs_file_readn(filp, buffer, NULL, PAGE_CACHE_SIZE, offset);
	if (retval < 0) {
		v9fs_uncache_page(inode, page);
		goto done;
	}

	memset(buffer + retval, 0, PAGE_CACHE_SIZE - retval);
	flush_dcache_page(page);
	SetPageUptodate(page);

	v9fs_readpage_to_fscache(inode, page);
	retval = 0;

done:
	kunmap(page);
	unlock_page(page);
	return retval;
}


static int v9fs_vfs_readpages(struct file *filp, struct address_space *mapping,
			     struct list_head *pages, unsigned nr_pages)
{
	int ret = 0;
	struct inode *inode;

	inode = mapping->host;
	P9_DPRINTK(P9_DEBUG_VFS, "inode: %p file: %p\n", inode, filp);

	ret = v9fs_readpages_from_fscache(inode, mapping, pages, &nr_pages);
	if (ret == 0)
		return ret;

	ret = read_cache_pages(mapping, pages, (void *)v9fs_vfs_readpage, filp);
	P9_DPRINTK(P9_DEBUG_VFS, "  = %d\n", ret);
	return ret;
}


static int v9fs_release_page(struct page *page, gfp_t gfp)
{
	if (PagePrivate(page))
		return 0;

	return v9fs_fscache_release_page(page, gfp);
}


static void v9fs_invalidate_page(struct page *page, unsigned long offset)
{
	if (offset == 0)
		v9fs_fscache_invalidate_page(page);
}


static int v9fs_launder_page(struct page *page)
{
	return 0;
}

const struct address_space_operations v9fs_addr_operations = {
      .readpage = v9fs_vfs_readpage,
      .readpages = v9fs_vfs_readpages,
      .releasepage = v9fs_release_page,
      .invalidatepage = v9fs_invalidate_page,
      .launder_page = v9fs_launder_page,
};
