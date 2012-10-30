/*
 * Ram backed block device driver.
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/smp_lock.h>
#include <linux/radix-tree.h>
#include <linux/buffer_head.h> /* invalidate_bh_lrus() */
#include <linux/slab.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/mm.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/pgtable_types.h>

#define NVRAMDISK_MAJOR		262
#define NUMOFSHADOW 32

#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)

/*
 * Each block ramdisk device has a radix_tree brd_pages of pages that stores
 * the pages containing the block device's contents. A brd page's ->index is
 * its offset in PAGE_SIZE units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */
 typedef struct shadow_variables
{
	unsigned long long l1;
	unsigned long long l2;
} shadow_variables;

struct manage_map
{
	unsigned long long magic;
	unsigned long long block_size;
	unsigned long long num_blocks;
	void* shadow_start, *shadow_end;
	void* start, *end;
	void* shadow[NUMOFSHADOW];	
	shadow_variables set[NUMOFSHADOW];
	
	struct list_head shadow_list;
	spinlock_t list_lock;	
};
struct shadow_list
{
	struct list_head list;
	int index;
};

struct nvdisk_device {
	int		nvdisk_number;
	int		nvdisk_refcnt;
	loff_t		nvdisk_offset;
	loff_t		nvdisk_sizelimit;
	unsigned	nvdisk_blocksize;

	struct request_queue	*nvdisk_queue;
	struct gendisk		*nvdisk_disk;
	struct list_head	nvdisk_list;

	/*
	 * Backing store of pages and lock to protect it. This is the contents
	 * of the block device.
	 */
	spinlock_t		nvdisk_lock;
	struct radix_tree_root	nvdisk_pages;
	//void * nvdisk_vaddr;
	struct manage_map* nvdisk_manage;
	//struct list_head brd_shadow;
};


/*
 * Look up and return a brd's page for a given sector.
 */
static struct page *nvdisk_lookup_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	pgoff_t idx;
	struct page *page;

	return 0;
}

/*
 * Look up and return a nvdisk's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */
static struct page *nvdisk_insert_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	pgoff_t idx;
	struct page *page;
	gfp_t gfp_flags;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support XIP and highmem, because our ->direct_access
	 * routine for XIP must return memory that is always addressable.
	 * If XIP was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */
	
	return 0;//page;
}

static void nvdisk_free_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	struct page *page;
	pgoff_t idx;

	spin_lock(&nvdisk->nvdisk_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page = radix_tree_delete(&nvdisk->nvdisk_pages, idx);
	spin_unlock(&nvdisk->nvdisk_lock);
	if (page)
		__free_page(page);
}

static void nvdisk_zero_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	struct page *page;

	page = nvdisk_lookup_page(nvdisk, sector);
	if (page)
		clear_highpage(page);
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void nvdisk_free_pages(struct nvdisk_device *nvdisk)
{
	unsigned long pos = 0;
	struct page *pages[FREE_BATCH];
	int nr_pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(&nvdisk->nvdisk_pages,
				(void **)pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			void *ret;

			BUG_ON(pages[i]->index < pos);
			pos = pages[i]->index;
			ret = radix_tree_delete(&nvdisk->nvdisk_pages, pos);
			BUG_ON(!ret || ret != pages[i]);
			__free_page(pages[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_pages == FREE_BATCH);
}

static void discard_from_nvdisk(struct nvdisk_device *nvdisk,
			sector_t sector, size_t n)
{
	while (n >= PAGE_SIZE) {
		/*
		 * Don't want to actually discard pages here because
		 * re-allocating the pages can result in writeback
		 * deadlocks under heavy load.
		 */
//		if (0)
//			nvdisk_free_page(nvdisk, sector);
//		else
//			nvdisk_zero_page(nvdisk, sector);
		sector += PAGE_SIZE >> SECTOR_SHIFT;
		n -= PAGE_SIZE;
	}
}

/*
 * Copy n bytes from src to the nvdisk starting at sector. Does not sleep.
 */
static void copy_to_nvdisk(struct nvdisk_device *nvdisk, const void *src,
			sector_t sector, size_t n)
{
	/*struct page *page;
	void *dst;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = nvdisk_lookup_page(nvdisk, sector);
	BUG_ON(!page);

	dst = kmap_atomic(page, KM_USER1);
	memcpy(dst + offset, src, copy);
	clflush_cache_range(dst, copy);
	kunmap_atomic(dst, KM_USER1);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = nvdisk_lookup_page(nvdisk, sector);
		BUG_ON(!page);

		dst = kmap_atomic(page, KM_USER1);
		memcpy(dst, src, copy);
		clflush_cache_range(dst, copy);
		kunmap_atomic(dst, KM_USER1);
	}*/
	//////////////////////////////

	
	//nvdisk_write(nvdisk, src, sector, n);
	//cycles_t clk_start, clk_end;
	//cycles_t clk_all, clk_tmp;
	pgd_t *pgd, *pgd_s;
	pud_t *pud, *pud_s;
	pmd_t * pmd, *pmd_s;
	pte_t * pte, *pte_s;
	pte_t pte_temp, pte_temp_s;
	//gfp_t gfp_mask = GFP_KERNEL | __GFP_HIGHMEM;
	pgprot_t prot = PAGE_KERNEL;//__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED);//;MEMSTOR_PROT;
	pgprot_t prot_s = PAGE_KERNEL;//__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED);;//MEMSTOR_PROT;//__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED); //|_PAGE_PWT); //PAGE_KERNEL_IO_WC;// 

	void * des;
	
	//_PAGE_PWT
	pte_t tmp;
	struct page * page_shadow;
	struct page * page_dest;
	
	
	struct shadow_list *working_list;
	int index;
	struct manage_map* map= nvdisk->nvdisk_manage;
	
	unsigned long long shadow_addr;
	unsigned long long des_addr;
	
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	
	void * start_addr = nvdisk->nvdisk_manage->start;
	void * dst;
		
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);

	//des = start_addr + ((sector>> PAGE_SECTORS_SHIFT) << PAGE_SHIFT);//////////////////////
	des = (unsigned long long)start_addr + (unsigned long long)((sector << SECTOR_SHIFT) & (~((1<<PAGE_SHIFT)-1)));
	dst = (void*) des;
	/////////////////////////test test test///////////////////////////
	/*printk(KERN_ALERT"memcpy1");
	memcpy(dst+offset, src, copy);
	printk(KERN_ALERT"memcpy2");
	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		des = start_addr + ((sector>> PAGE_SECTORS_SHIFT) << PAGE_SHIFT);//////////////////////
		dst = (void*) des;
		printk(KERN_ALERT"memcpy3");
		memcpy(dst, src, copy);
		printk(KERN_ALERT"memcpy4");

	}
	return;
	printk(KERN_ALERT"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");*/
	/////////////////////////test test test///////////////////////////

	des_addr= (unsigned long long)des;
	
	/*if(n != 4096)
	{
//		pram_warn("no atomic write path");
		if (memcpy((void*)des_addr, src, n)) {
		
			return 0;
		
		}
		//memcpy(des_addr, src, size);
		return;
	}*/

	pgd = pgd_offset_k(des_addr);
	pud = pud_alloc(&init_mm, pgd, des_addr);
	pmd = pmd_alloc(&init_mm, pud, des_addr);
	pte = pte_alloc_kernel(pmd, des_addr);


	pte_temp = *pte;
	page_dest = pte_page(pte_temp);//get page descriptor of des addr
	if(page_dest == NULL)
	{
		printk(KERN_ALERT"nvrdisk: page_dest NULL");
//		pram_err("page_dest is NULL\n");
		page_dest = alloc_page( GFP_KERNEL/* | __GFP_NVRAM*/);
		
		
//		memcpy(__va(PFN_PHYS(page_to_pfn(page_dest))), src, size);
//		clflush_cache_range((void*)des_addr, 4096);
		set_pte_at(&init_mm, des_addr, pte, mk_pte(page_dest, prot_s));
//		clflush_cache_range(pte, sizeof(pte_t));
		return;		
	}
	else
		tmp = mk_pte(page_dest, prot_s);
	

	//down(&shadow_sem);
	spin_lock(&map->list_lock);
	if(list_empty(&map->shadow_list))
	{
		printk(KERN_ALERT"shadow block is not available");
		spin_unlock(&map->list_lock);
		return;
		//spin_lock(&map->list_lock);
		//return 0;
		//spin_unlock_wait(&map->list_lock);
	}
	else
	{
		//spin_lock(&map->list_lock);
		working_list = list_first_entry(&map->shadow_list, struct shadow_list, list);
		index=working_list->index;		
		list_del(&working_list->list);
		spin_unlock(&map->list_lock);
	}
	
	//clk_end = get_cycles();
	//pram_info("Shadow Get = %ld\n",clk_end-clk_start);
	
	
	shadow_addr = map->shadow[index];
	//clk_start = get_cycles();	
	pgd_s = pgd_offset_k(shadow_addr);
	pud_s = pud_alloc(&init_mm, pgd_s, shadow_addr);
	pmd_s = pmd_alloc(&init_mm, pud_s, shadow_addr);
	pte_s = pte_alloc_kernel(pmd_s, shadow_addr);
	pte_temp_s = *pte_s;	
	page_shadow = pte_page(pte_temp_s);
	//clk_end = get_cycles();
	//pram_info("page table manipulation = %ld\n",clk_end-clk_start);
		
	map->set[index].l1 = des_addr;
	//wmb();
	dst = __va(PFN_PHYS(page_to_pfn(page_shadow)));
	//printk(KERN_ALERT"-----------Before-------------");	
	//printk(KERN_ALERT"des_addr=%x", des);	
	//printk(KERN_ALERT"va(des_addr)=%x", __va(PFN_PHYS(page_to_pfn(page_dest))));
	//printk(KERN_ALERT"shadow_addr=%x", shadow_addr);
	//printk(KERN_ALERT"va(shadow_addr)=%x", dst);
	
	memcpy(dst, __va(PFN_PHYS(page_to_pfn(page_dest))), offset);
	//memcpy(__va(PFN_PHYS(page_to_pfn(page_shadow)))+offset, src, copy);
	memcpy(dst + offset, src, copy);
	memcpy(dst + offset + copy, __va(PFN_PHYS(page_to_pfn(page_dest)))+offset+copy, PAGE_SIZE-(copy+offset));
	//__inline_memcpy((void*)shadow_addr, src, size);
	//clk_end = get_cycles();
	//pram_info("memcpy() = %ld\n",clk_end-clk_start);
	//clk_start = get_cycles();	
	clflush_cache_range(dst, 4096);
	
	map->set[index].l2 = des_addr;
	clflush_cache_range(&map->set[index], sizeof(shadow_variables));
	set_pte_at(&init_mm, des_addr, pte, mk_pte(page_shadow, prot));
	clflush_cache_range(pte, sizeof(pte_t));
	map->set[index].l2=0;
	clflush_cache_range(&map->set[index], sizeof(shadow_variables));
	map->set[index].l1=0;
	//spin_lock(&init_mm.page_table_lock);
	set_pte_at(&init_mm, shadow_addr, pte_s, tmp);
	
	spin_lock(&map->list_lock);
	list_add(&working_list->list, &map->shadow_list);
	spin_unlock(&map->list_lock);
	//printk(KERN_ALERT"-----------AFTER-------------");	
	//printk(KERN_ALERT"des_addr=%x", des);	
	//printk(KERN_ALERT"=%x", __va(PFN_PHYS(page_to_pfn(page_dest))));
	//printk(KERN_ALERT"shadow_addr=%x", shadow_addr);
	//printk(KERN_ALERT"dst=%x", __va(PFN_PHYS(page_to_pfn(page_shadow))));
	//up(&shadow_sem);
	
	return;
	
	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		//page = nvisk_lookup_page(nvdisk, sector);
		/*BUG_ON(!page);

		dst = kmap_atomic(page, KM_USER1);
		memcpy(dst, src, copy);
		clflush_cache_range(dst, copy);
		kunmap_atomic(dst, KM_USER1);*/
		des = start_addr + (unsigned long long)((sector << SECTOR_SHIFT) & (~((1<<PAGE_SHIFT)-1)));//////////////////////

		des_addr= (unsigned long long)des;
		
		/*if(n != 4096)
		{
	//		pram_warn("no atomic write path");
			if (memcpy((void*)des_addr, src, size)) {
			
				return 0;
			
			}
			//memcpy(des_addr, src, size);
			return;
		}*/

		pgd = pgd_offset_k(des_addr);
		pud = pud_alloc(&init_mm, pgd, des_addr);
		pmd = pmd_alloc(&init_mm, pud, des_addr);
		pte = pte_alloc_kernel(pmd, des_addr);


		pte_temp = *pte;
		page_dest = pte_page(pte_temp);//get page descriptor of des addr
		if(page_dest == NULL)
		{
	//		pram_err("page_dest is NULL\n");
			page_dest = alloc_page( GFP_KERNEL/* | __GFP_NVRAM*/);
			
			
	//		memcpy(__va(PFN_PHYS(page_to_pfn(page_dest))), src, size);
	//		clflush_cache_range((void*)des_addr, 4096);
			set_pte_at(&init_mm, des_addr, pte, mk_pte(page_dest, prot_s));
	//		clflush_cache_range(pte, sizeof(pte_t));
			return;		
		}
		else
			tmp = mk_pte(page_dest, prot_s);
		

		//down(&shadow_sem);
		spin_lock(&map->list_lock);
		if(list_empty(&map->shadow_list))
		{
	//		pram_err("shadow block is not available\n");
			spin_unlock(&map->list_lock);
			return;
			//spin_lock(&map->list_lock);
			//return 0;
			//spin_unlock_wait(&map->list_lock);
		}
		else
		{
			//spin_lock(&map->list_lock);
			working_list = list_first_entry(&map->shadow_list, struct shadow_list, list);
			index=working_list->index;		
			list_del(&working_list->list);
			spin_unlock(&map->list_lock);
		}
		
		//clk_end = get_cycles();
		//pram_info("Shadow Get = %ld\n",clk_end-clk_start);
		
		
		shadow_addr = map->shadow[index];
		//clk_start = get_cycles();	
		pgd_s = pgd_offset_k(shadow_addr);
		pud_s = pud_alloc(&init_mm, pgd_s, shadow_addr);
		pmd_s = pmd_alloc(&init_mm, pud_s, shadow_addr);
		pte_s = pte_alloc_kernel(pmd_s, shadow_addr);
		pte_temp_s = *pte_s;	
		page_shadow = pte_page(pte_temp_s);
		//clk_end = get_cycles();
		//pram_info("page table manipulation = %ld\n",clk_end-clk_start);
			
		map->set[index].l1 = des_addr;
		//wmb();
		dst = __va(PFN_PHYS(page_to_pfn(page_shadow)));
		memcpy(dst, src, copy);
		memcpy(dst + copy, __va(PFN_PHYS(page_to_pfn(page_dest)))+copy, PAGE_SIZE-copy);
		//__inline_memcpy((void*)shadow_addr, src, size);
		//clk_end = get_cycles();
		//pram_info("memcpy() = %ld\n",clk_end-clk_start);
		//clk_start = get_cycles();	
		clflush_cache_range(shadow_addr, 4096);
		
		map->set[index].l2 = des_addr;
		clflush_cache_range(&map->set[index], sizeof(shadow_variables));
		set_pte_at(&init_mm, des_addr, pte, mk_pte(page_shadow, prot));
		clflush_cache_range(pte, sizeof(pte_t));
		map->set[index].l2=0;
		clflush_cache_range(&map->set[index], sizeof(shadow_variables));
		map->set[index].l1=0;
		//spin_lock(&init_mm.page_table_lock);
		set_pte_at(&init_mm, shadow_addr, pte_s, tmp);
		
		spin_lock(&map->list_lock);
		list_add(&working_list->list, &map->shadow_list);
		spin_unlock(&map->list_lock);
		
	}

	
}

/*
 * Copy n bytes to dst from the nvdisk starting at sector. Does not sleep.
 */
static void copy_from_nvdisk(void *dst, struct nvdisk_device *nvdisk,
			sector_t sector, size_t n)
{
	/*struct page *page;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = nvdisk_lookup_page(nvdisk, sector);
	if (page) {
		src = kmap_atomic(page, KM_USER1);
		memcpy(dst, src + offset, copy);
		kunmap_atomic(src, KM_USER1);
	} else
		memset(dst, 0, copy);

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = nvdisk_lookup_page(nvdisk, sector);
		if (page) {
			src = kmap_atomic(page, KM_USER1);
			memcpy(dst, src, copy);
			kunmap_atomic(src, KM_USER1);
		} else
			memset(dst, 0, copy);
	}*/
	///////////////////////////////////////////////////////////
	
	pgd_t *pgd;
	pud_t *pud;
	pmd_t * pmd;
	pte_t * pte;
	pte_t pte_temp;
	unsigned long long addr;
	struct page * page_src;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	//printk(KERN_ALERT"nvdisk: offset = %x", offset);
	//addr = nvdisk->nvdisk_manage->start + ((sector>> PAGE_SECTORS_SHIFT) << PAGE_SHIFT);// & (~0x111); 
	addr = (unsigned long long)nvdisk->nvdisk_manage->start + (unsigned long long)((sector << SECTOR_SHIFT) & (~((1<<PAGE_SHIFT)-1)));
	//printk(KERN_ALERT"nvdisk: addr = %x", addr);
	src = (void*)addr;
	//printk(KERN_ALERT"nvdisk: src = %x", src);

	/////////////////////////test test test///////////////////////////
	/*printk(KERN_ALERT"memcpy5");
	memcpy(dst, src + offset, copy);
	printk(KERN_ALERT"memcpy6");
	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		addr = nvdisk->nvdisk_manage->start + ((sector>> PAGE_SECTORS_SHIFT) << PAGE_SHIFT);// & (~0x111); 
		if (addr) {		
			printk(KERN_ALERT"memcpy7");
			memcpy(dst, addr, copy);
			printk(KERN_ALERT"memcpy8");
		
		} else
			memset(dst, 0, copy);
	}
	return;
	printk(KERN_ALERT"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	*/
	/////////////////////////test test test///////////////////////////
	
	pgd = pgd_offset_k(addr);
	pud = pud_alloc(&init_mm, pgd, addr);
	pmd = pmd_alloc(&init_mm, pud, addr);
	pte = pte_alloc_kernel(pmd, addr);
	pte_temp = *pte;
	
	page_src = pte_page(pte_temp);//get page descriptor of des addr
	src = __va(PFN_PHYS(page_to_pfn(page_src)));
	if(src)
		memcpy(dst, src+offset, copy);
	else
		memset(dst, 0, copy);
	
	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		//page = nvdisk_lookup_page(nvdisk, sector);

		addr = nvdisk->nvdisk_manage->start + (unsigned long long)((sector << SECTOR_SHIFT) & (~((1<<PAGE_SHIFT)-1)));// & (~0x111); 
		
		pgd = pgd_offset_k(addr);
		pud = pud_alloc(&init_mm, pgd, addr);
		pmd = pmd_alloc(&init_mm, pud, addr);
		pte = pte_alloc_kernel(pmd, addr);
		pte_temp = *pte;
		
		page_src = pte_page(pte_temp);//get page descriptor of des addr
		if(src = __va(PFN_PHYS(page_to_pfn(page_src))))
			memcpy(dst, src, copy);
		else
			memset(dst, 0, copy);			


		/*if (page) {
			src = kmap_atomic(page, KM_USER1);
			memcpy(dst, src, copy);
			kunmap_atomic(src, KM_USER1);
		} else
			memset(dst, 0, copy);*/
	}
}

/*
 * Process a single bvec of a bio.
 */
static int nvdisk_do_bvec(struct nvdisk_device *nvdisk, struct page *page,
			unsigned int len, unsigned int off, int rw,
			sector_t sector)
{
	void *mem;
	int err = 0;
	
	mem = kmap_atomic(page, KM_USER0);
	if (rw == READ) {
		copy_from_nvdisk(mem + off, nvdisk, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_nvdisk(nvdisk, mem + off, sector, len);
	}
	kunmap_atomic(mem, KM_USER0);

out:
	return err;
}

static int nvdisk_make_request(struct request_queue *q, struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	struct nvdisk_device *nvdisk = bdev->bd_disk->private_data;
	int rw;
	struct bio_vec *bvec;
	sector_t sector;
	int i;
	int err = -EIO;

	sector = bio->bi_sector;
	if (sector + (bio->bi_size >> SECTOR_SHIFT) >
						get_capacity(bdev->bd_disk))
		goto out;

	if (unlikely(bio->bi_rw & REQ_DISCARD)) {
		err = 0;
		discard_from_nvdisk(nvdisk, sector, bio->bi_size);
		goto out;
	}

	rw = bio_rw(bio);
	if (rw == READA)
		rw = READ;

	bio_for_each_segment(bvec, bio, i) {
		unsigned int len = bvec->bv_len;
		err = nvdisk_do_bvec(nvdisk, bvec->bv_page, len,
					bvec->bv_offset, rw, sector);
		if (err)
			break;
		sector += len >> SECTOR_SHIFT;
	}

out:
	bio_endio(bio, err);

	return 0;
}

#ifdef CONFIG_BLK_DEV_XIP
static int nvdisk_direct_access(struct block_device *bdev, sector_t sector,
			void **kaddr, unsigned long *pfn)
{
	struct nvdisk_device *nvdisk = bdev->bd_disk->private_data;
	struct page *page;

	if (!nvdisk)
		return -ENODEV;
	if (sector & (PAGE_SECTORS-1))
		return -EINVAL;
	if (sector + PAGE_SECTORS > get_capacity(bdev->bd_disk))
		return -ERANGE;
	page = nvdisk_insert_page(nvdisk, sector);
	if (!page)
		return -ENOMEM;
	*kaddr = page_address(page);
	*pfn = page_to_pfn(page);

	return 0;
}
#endif

static int nvdisk_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	int error;
	struct nvdisk_device *nvdisk = bdev->bd_disk->private_data;

	if (cmd != BLKFLSBUF)
		return -ENOTTY;

	/*
	 * ram device BLKFLSBUF has special semantics, we want to actually
	 * release and destroy the ramdisk data.
	 */
	lock_kernel();
	mutex_lock(&bdev->bd_mutex);
	error = -EBUSY;
	if (bdev->bd_openers <= 1) {
		/*
		 * Invalidate the cache first, so it isn't written
		 * back to the device.
		 *
		 * Another thread might instantiate more buffercache here,
		 * but there is not much we can do to close that race.
		 */
		invalidate_bh_lrus();
		truncate_inode_pages(bdev->bd_inode->i_mapping, 0);
		nvdisk_free_pages(nvdisk);
		error = 0;
	}
	mutex_unlock(&bdev->bd_mutex);
	unlock_kernel();

	return error;
}

static const struct block_device_operations nvdisk_fops = {
	.owner =		THIS_MODULE,
	.ioctl =		nvdisk_ioctl,
#ifdef CONFIG_BLK_DEV_XIP
	.direct_access =	nvdisk_direct_access,
#endif
};

/*
 * And now the modules code and kernel interface.
 */
static int nvrd_nr;
int nvrd_size = 1024*1024*2;//CONFIG_BLK_DEV_RAM_SIZE;
static int max_part;
static int part_shift;
module_param(nvrd_nr, int, 0);
MODULE_PARM_DESC(nvrd_nr, "Maximum number of nvdisk devices");
module_param(nvrd_size, int, 0);
MODULE_PARM_DESC(nvrd_size, "Size of each RAM disk in kbytes.");
module_param(max_part, int, 0);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per RAM disk");
MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(NVRAMDISK_MAJOR);
MODULE_ALIAS("rd");

#ifndef MODULE
/* Legacy boot options - nonmodular */
static int __init ramdisk_size(char *str)
{
	nvrd_size = simple_strtol(str, NULL, 0);
	return 1;
}
__setup("nvramdisk_size=", ramdisk_size);
#endif

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(nvdisk_devices);
static DEFINE_MUTEX(nvdisk_devices_mutex);

static struct nvdisk_device *nvdisk_alloc(int i)
{
	struct nvdisk_device *nvdisk;
	struct gendisk *disk;
	
	struct manage_map* nvdisk_manage;
	void * shadow_start;
	
	nvdisk = kzalloc(sizeof(*nvdisk), GFP_KERNEL);
	if (!nvdisk)
		goto out;
	nvdisk->nvdisk_number		= i;
	spin_lock_init(&nvdisk->nvdisk_lock);
	INIT_RADIX_TREE(&nvdisk->nvdisk_pages, GFP_ATOMIC);

	nvdisk->nvdisk_queue = blk_alloc_queue(GFP_KERNEL);
	if (!nvdisk->nvdisk_queue)
		goto out_free_dev;
	blk_queue_make_request(nvdisk->nvdisk_queue, nvdisk_make_request);
	blk_queue_ordered(nvdisk->nvdisk_queue, QUEUE_ORDERED_TAG);
	blk_queue_max_hw_sectors(nvdisk->nvdisk_queue, 1024);
	blk_queue_bounce_limit(nvdisk->nvdisk_queue, BLK_BOUNCE_ANY);

	nvdisk->nvdisk_queue->limits.discard_granularity = PAGE_SIZE;
	nvdisk->nvdisk_queue->limits.max_discard_sectors = UINT_MAX;
	nvdisk->nvdisk_queue->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, nvdisk->nvdisk_queue);

	disk = nvdisk->nvdisk_disk = alloc_disk(1 << part_shift);
	if (!disk)
		goto out_free_queue;
	disk->major		= NVRAMDISK_MAJOR;
	disk->first_minor	= i << part_shift;
	disk->fops		= &nvdisk_fops;
	disk->private_data	= nvdisk;
	disk->queue		= nvdisk->nvdisk_queue;
	disk->flags |= GENHD_FL_SUPPRESS_PARTITION_INFO;
	sprintf(disk->disk_name, "nvram%d", i);
	set_capacity(disk, nvrd_size/* *2 */);
	
	printk(KERN_ALERT"nvdisk: manage_map alloc");
	nvdisk_manage = kzalloc(sizeof(struct manage_map), GFP_ATOMIC);
	nvdisk->nvdisk_manage = nvdisk_manage;
	nvdisk->nvdisk_manage->start = vmalloc(nvrd_size*512);
	printk(KERN_ALERT"nvdisk: partition alloc %x", nvdisk->nvdisk_manage->start);
	nvdisk->nvdisk_manage->	shadow_start = vmalloc(PAGE_SIZE * NUMOFSHADOW);
	printk(KERN_ALERT"nvdisk: shadow block alloc %x", nvdisk->nvdisk_manage->shadow_start);
	printk(KERN_ALERT"nvdisk: list init");
	INIT_LIST_HEAD(&nvdisk_manage->shadow_list);
	printk(KERN_ALERT"nvdisk: list for init");
	for(i=0; i<NUMOFSHADOW; i++)
	{
		struct shadow_list * tmp_list;
		void* shadow_start = nvdisk->nvdisk_manage->	shadow_start;
		tmp_list = (struct shadow_list*)kmalloc(sizeof(struct shadow_list), GFP_ATOMIC);
		tmp_list->index=i;
		list_add( &tmp_list->list, &nvdisk_manage->shadow_list);
		nvdisk_manage->shadow[i] = shadow_start + PAGE_SIZE * i;
		//printk("Shadow Block %d in %llX\n", i, nvdisk->nvdisk_shadow[i]);
		nvdisk_manage->set[i].l1 = 0;
		nvdisk_manage->set[i].l2 = 0;		
	}
	printk(KERN_ALERT"nvdisk: spinlock init");
	spin_lock_init(&nvdisk->nvdisk_manage->list_lock);

	return nvdisk;

out_free_queue:
	blk_cleanup_queue(nvdisk->nvdisk_queue);
out_free_dev:
	kfree(nvdisk);
out:
	return NULL;
}

static void nvdisk_free(struct nvdisk_device *nvdisk)
{
	put_disk(nvdisk->nvdisk_disk);
	blk_cleanup_queue(nvdisk->nvdisk_queue);
	nvdisk_free_pages(nvdisk);
	kfree(nvdisk);
}

static struct nvdisk_device *nvdisk_init_one(int i)
{
	struct nvdisk_device *nvdisk;

	list_for_each_entry(nvdisk, &nvdisk_devices, nvdisk_list) {
		if (nvdisk->nvdisk_number == i)
			goto out;
	}

	nvdisk = nvdisk_alloc(i);
	if (nvdisk) {
		add_disk(nvdisk->nvdisk_disk);
		list_add_tail(&nvdisk->nvdisk_list, &nvdisk_devices);
	}
out:
	return nvdisk;
}

static void nvdisk_del_one(struct nvdisk_device *nvdisk)
{
	list_del(&nvdisk->nvdisk_list);
	del_gendisk(nvdisk->nvdisk_disk);
	nvdisk_free(nvdisk);
}

static struct kobject *nvdisk_probe(dev_t dev, int *part, void *data)
{
	struct nvdisk_device *nvdisk;
	struct kobject *kobj;

	mutex_lock(&nvdisk_devices_mutex);
	nvdisk = nvdisk_init_one(dev & MINORMASK);
	kobj = nvdisk ? get_disk(nvdisk->nvdisk_disk) : ERR_PTR(-ENOMEM);
	mutex_unlock(&nvdisk_devices_mutex);

	*part = 0;
	return kobj;
}

static int __init nvdisk_init(void)
{
	int i, nr;
	unsigned long range;
	struct nvdisk_device *nvdisk, *next;

	/*
	 * nvdisk module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 * However, this will not work well with user space tool that doesn't
	 * know about such "feature".  In order to not break any existing
	 * tool, we do the following:
	 *
	 * (1) if nvrd_nr is specified, create that many upfront, and this
	 *     also becomes a hard limit.
	 * (2) if nvrd_nr is not specified, create 1 rd device on module
	 *     load, user can further extend nvdisk device by create dev node
	 *     themselves and have kernel automatically instantiate actual
	 *     device on-demand.
	 */
	nvrd_nr = 1;
//	nvrd_size = 1024*1024*2;
	
	part_shift = 0;
	if (max_part > 0)
		part_shift = fls(max_part);

	if (nvrd_nr > 1UL << (MINORBITS - part_shift))
		return -EINVAL;

	if (nvrd_nr) {
		nr = nvrd_nr;
		range = nvrd_nr;
	} else {
		nr = CONFIG_BLK_DEV_RAM_COUNT;
		range = 1UL << (MINORBITS - part_shift);
	}

	if (register_blkdev(NVRAMDISK_MAJOR, "nvramdisk"))
		return -EIO;

	for (i = 0; i < nr; i++) {
		nvdisk = nvdisk_alloc(i);
		if (!nvdisk)
			goto out_free;
		list_add_tail(&nvdisk->nvdisk_list, &nvdisk_devices);
	}

	/* point of no return */

	list_for_each_entry(nvdisk, &nvdisk_devices, nvdisk_list)
		add_disk(nvdisk->nvdisk_disk);

	blk_register_region(MKDEV(NVRAMDISK_MAJOR, 0), range,
				  THIS_MODULE, nvdisk_probe, NULL, NULL);

	printk(KERN_INFO "nvdisk: module loaded\n");
	return 0;

out_free:
	list_for_each_entry_safe(nvdisk, next, &nvdisk_devices, nvdisk_list) {
		list_del(&nvdisk->nvdisk_list);
		nvdisk_free(nvdisk);
	}
	unregister_blkdev(NVRAMDISK_MAJOR, "nvramdisk");

	return -ENOMEM;
}

static void __exit nvdisk_exit(void)
{
	unsigned long range;
	struct nvdisk_device *nvdisk, *next;

	range = nvrd_nr ? nvrd_nr :  1UL << (MINORBITS - part_shift);

	list_for_each_entry_safe(nvdisk, next, &nvdisk_devices, nvdisk_list)
		nvdisk_del_one(nvdisk);

	blk_unregister_region(MKDEV(NVRAMDISK_MAJOR, 0), range);
	unregister_blkdev(NVRAMDISK_MAJOR, "nvramdisk");
}

module_init(nvdisk_init);
module_exit(nvdisk_exit);

