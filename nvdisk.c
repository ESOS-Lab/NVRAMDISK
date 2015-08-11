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
//#include <linux/smp_lock.h>
#include <linux/radix-tree.h>
#include <linux/buffer_head.h> /* invalidate_bh_lrus() */
#include <linux/slab.h>

#include <linux/spinlock.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/mm.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/pgtable_types.h>
#include <asm/tsc.h>
#include <linux/preempt.h>
#include <asm/pgtable_types.h>
#include <asm/io.h>
#include <asm/tlbflush.h>
#include <linux/page-flags.h>
#include <linux/delay.h>
#include "nvdisk.h"

//#define NVRAMDISK_TIMECHECK
//#define NVRAMDISK_DEBUG


//#define NVRAMDISK_BLOCK_WB
//#define NVRAMDISK_BLOCK_CLFLUSH
#define NVRAMDISK_BLOCK_MOVNTQ
//#define NVRAMDISK_BLOCK_WT
//#define NVRAMDISK_BLOCK_WC
//#define NVRAMDISK_BLOCK_UC


//#define NVRAMDISK_MAPPING_TABLE_WB
//#define NVRAMDISK_MAPPING_TABLE_CLFLUSH
#define NVRAMDISK_MAPPING_TABLE_MOVNTQ
//#define NVRAMDISK_MAPPING_TABLE_WT
//#define NVRAMDISK_MAPPING_TABLE_WC
//#define NVRAMDISK_MAPPING_TABLE_UC


//#define NVRAMDISK_LVTABLE_WB
//#define NVRAMDISK_LVTABLE_CLFLUSH
//#define NVRAMDISK_LVTABLE_WC
//#define NVRAMDISK_LVTABLE_WT
//#define NVRAMDISK_LVTABLE_UC
#define NVRAMDISK_LVTABLE_MOVNTQ



#define SHADOW_BLOCK_ENABLE



#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)



//#define NVRAMDISK_LOG_WB
//#define NVRAMDISK_LOG_CLFLUSH
//#define NVRAMDISK_LOG_WC
//#define NVRAMDISK_LOG_WT
//#define NVRAMDISK_LOG_UC
#define NVRAMDISK_LOG_MOVNTQ




/*
 * Each block ramdisk device has a radix_tree brd_pages of pages that stores
 * the pages containing the block device's contents. A brd page's ->index is
 * its offset in PAGE_SIZE units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */

//int subset_lock_init(struct nvdisk_device * nvdisk);

//int nvramdisk_mapping_table_init(struct nvdisk_device* nvdisk);



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

/*static inline void asm_movnti(volatile pcm_word_t *addr, pcm_word_t val)
{
	__asm__ __volatile__ ("movnti %1, %0" : "=m"(*addr): "r" (val));
	__asm__ ("mfence");
}*/
int number_of_bits(int n) {
    int l = 0;
    while (n) {
        n = n / 2;
        l++;
    }
    return l-1;
}

//static inline void nvdisk_log_update2(struct log_entry* log_entry, unsigned int lv, unsigned int value){}


static inline void nvdisk_log_update(void** entry, void** value)//nvdisk_table_update(&primary_table[idx], &dst);
{
#ifdef NVRAMDISK_LVTABLE_MOVNTQ
	__asm__ __volatile__(
		//"prefetchnta (%0)\n"
		"movq (%0), %%mm0\n"
		"movntq %%mm0, (%1)\n"
		: :"r" (value), "r"(entry) : "memory");

#else//NVRAMDISK_LVTABLE_WT || NVRAMDISK_LVTABLE_CLFLUSH || NVRAMDISK_LVTABLE_WB || NVRAMDISK_LVTABLE_WC || NVRAMDISK_LVTABLE_UC
	*entry=*value;
#endif

#ifdef NVRAMDISK_LVTABLE_CLFLUSH
	clflush_cache_range(entry, sizeof(nvramdisk_mapping_table_entry));		
 #endif
 #if (defined NVRAMDISK_LVTABLE_MOVNTQ) || (defined NVRAMDISK_LVTABLE_WC) 
	__asm__ __volatile__("sfence");
#else// NVRAMDISK_LVTABLE_WT || NVRAMDISK_LVTABLE_WB || NVRAMDISK_LVTABLE_UC
	;
#endif
}

static inline void nvdisk_table_update(void** entry, void** value)//nvdisk_table_update(&primary_table[idx], &dst);
{
#ifdef NVRAMDISK_MAPPING_TABLE_MOVNTQ
	__asm__ __volatile__(
		//"prefetchnta (%0)\n"
		"movq (%0), %%mm0\n"
		"movntq %%mm0, (%1)\n"
		: :"r" (value), "r"(entry) : "memory");

#else//NVRAMDISK_MAPPING_TABLE_WT || NVRAMDISK_MAPPING_TABLE_CLFLUSH || NVRAMDISK_MAPPING_TABLE_WB || NVRAMDISK_MAPPING_TABLE_WC || NVRAMDISK_MAPPING_TABLE_UC
	*entry=*value;
#endif

#ifdef NVRAMDISK_MAPPING_TABLE_CLFLUSH
	clflush_cache_range(entry, sizeof(nvramdisk_mapping_table_entry));		
 #endif
 #if (defined NVRAMDISK_MAPPING_TABLE_MOVNTQ) || (defined NVRAMDISK_MAPPING_TABLE_WC) //NVRAMDISK_MAPPING_TABLE_MOVNTQ || NVRAMDISK_MAPPING_TABLE_WC
	__asm__ __volatile__("mfence");
#else// NVRAMDISK_MAPPING_TABLE_WT || NVRAMDISK_MAPPING_TABLE_WB || NVRAMDISK_MAPPING_TABLE_UC
	;
#endif
}

static inline void nvdisk_native_flush_tlb_single(unsigned long addr)
{
	asm volatile("invlpg (%0)" ::"r" (addr) : "memory");
}
/*bool set_kernel_page_prot(struct page *page, pgprot_t pgprot)
{
	unsigned int level;
	pte_t *pte;
	pgprotval_t protval = pgprot_val(pgprot);
	
	unsigned long pfn = __page_to_pfn(page);
	//__pa(addr) >> PAGE_SHIFT;
	
	
	//pteval_t val;
	if (PageHighMem(page))
		return false;
	
	

	//if (protval & _PAGE_PRESENT)
	//	protval &= __supported_pte_mask;
	pte = lookup_address((unsigned long)page_address(page), &level);

	if(pfn != native_pte_val(*pte) & PTE_FLAG_MASK)
		printk(KERN_ERR"PHYS not match");
	
	*pte = pfn_pte(pfn, pgprot);
	//val = pte_val(*pte);
	//val = val | pgprot;	

	//if(protval & _PAGE_PWT)
	//	*pte = __pte(pte_val(*pte) | protval);
	//else
	//	*pte = __pte(pte_val(*pte) & ~_PAGE_PWT);
	nvdisk_native_flush_tlb_single(page_address(page));
	return 0;//(pte_val(*pte) & _PAGE_PRESENT);
}*/



#ifdef NVRAMDISK_MEMCPY

void* memcpy_arjanv_movntq (void *to, const void *from, size_t len)
{
	int i;

	__asm__ __volatile__ (
		"1: prefetchnta (%0)\n"
		"   prefetchnta 64(%0)\n"
		"   prefetchnta 128(%0)\n"
		"   prefetchnta 192(%0)\n"
		: : "r" (from) );

	for(i=0; i<len/64; i++) {
		__asm__ __volatile__ (
			"   prefetchnta 200(%0)\n"
			"   movq (%0), %%mm0\n"
			"   movq 8(%0), %%mm1\n"
			"   movq 16(%0), %%mm2\n"
			"   movq 24(%0), %%mm3\n"
			"   movq 32(%0), %%mm4\n"
			"   movq 40(%0), %%mm5\n"
			"   movq 48(%0), %%mm6\n"
			"   movq 56(%0), %%mm7\n"
			"   movntq %%mm0, (%1)\n"
			"   movntq %%mm1, 8(%1)\n"
			"   movntq %%mm2, 16(%1)\n"
			"   movntq %%mm3, 24(%1)\n"
			"   movntq %%mm4, 32(%1)\n"
			"   movntq %%mm5, 40(%1)\n"
			"   movntq %%mm6, 48(%1)\n"
			"   movntq %%mm7, 56(%1)\n"
			: : "r" (from), "r" (to) : "memory");
		from+=64;
		to+=64;
	}
	/*
	 *Now do the tail of the block
	 */
	if (len&63)
	{
		memcpy(to, from, len&63);
		clflush_cache_range(to, len&63);
	}	
	return to;
}
#endif

void inline tslock(struct nvdisk_device *nvdisk, int idx)
{
	//int i;
	//i = idx / ( cache_line_size() / sizeof(nvramdisk_mapping_table_entry));
#ifdef NVRAMDISK_FINE_GRAINED_LOCKING
	spin_lock(&nvdisk->nvdisk_manage->table_subset_locks[idx]);
#else
	spin_lock(&nvdisk->nvdisk_manage->nvramdisk_lock);
#endif
#ifdef NVRAMDISK_DEBUG
//			printk(KERN_INFO"nvdisk: tslock\n");
#endif
	
}
static void inline tsunlock(struct nvdisk_device *nvdisk, int idx)
{
	//int i;
	//i = idx / ( cache_line_size() / sizeof(nvramdisk_mapping_table_entry));
#ifdef NVRAMDISK_FINE_GRAINED_LOCKING
	spin_unlock(&nvdisk->nvdisk_manage->table_subset_locks[idx]);
#else
	spin_unlock(&nvdisk->nvdisk_manage->nvramdisk_lock);
#endif

#ifdef NVRAMDISK_DEBUG
//			printk(KERN_INFO"nvdisk: tsunlock\n");
#endif
}
static void lv_table_destory(struct nvdisk_device* nvdisk)
{
	//kfree(nvdisk->nvdisk_manage->lv_table);
	struct page *page;
	int order;
	int lv_table_size;
	int lv_table_size_in_page;	
	int i;
	lv_table_size = NUMOFSHADOW*4*sizeof(unsigned long);
	lv_table_size_in_page = lv_table_size  / PAGE_SIZE;
	if(lv_table_size&((1<<PAGE_SHIFT)-1))
		lv_table_size_in_page++;
	
	order = number_of_bits(lv_table_size_in_page);		

	page = virt_to_page(nvdisk->nvdisk_manage->lv_tableold);
#if (defined NVRAMDISK_LVTABLE_UC) || (defined NVRAMDISK_LVTABLE_WT)
	for(i=0; i <(1<<order); i++)
		ClearPageReserved(page+i);
	iounmap(nvdisk->nvdisk_manage->lv_table);
#endif
	free_pages(nvdisk->nvdisk_manage->lv_tableold, order);
}
static void lv_table_init(struct nvdisk_device* nvdisk)
{
	int cache_line_size;
	int data_line_size;
	int channel;
	int word_size;
	int memio_unit;
	int imax, jmax;
	int i,j,n;
	int total_size;
	unsigned int lv_table_size;
	unsigned int lv_table_size_in_page;	
	int order;
	struct page* page;
	//total_size = NUMOFSHADOW*3;
	lvalue (*lv_table)[4];	
	cache_line_size = cache_line_size();
	data_line_size=8;//64bit
	channel = 2;
	memio_unit = data_line_size * channel;
	word_size = sizeof(unsigned long);

	jmax = cache_line_size / memio_unit;
	imax = memio_unit / word_size;	
	
	
	lv_table_size = NUMOFSHADOW*4*sizeof(lvalue);
	
	
	lv_table_size_in_page = lv_table_size  /PAGE_SIZE;	
	if(lv_table_size&((1<<PAGE_SHIFT)-1))
		lv_table_size_in_page++;
	
	order = number_of_bits(lv_table_size_in_page);
	
	//order++;
	if(nvdisk->nvdisk_manage->lv_tableold)
	{
		page=virt_to_page(nvdisk->nvdisk_manage->lv_tableold);
	}
	else
	{
		page = alloc_pages(GFP_KERNEL, order);
		nvdisk->nvdisk_manage->lv_tableold = page_address(page);
	}
	
	if(!page)
		printk(KERN_ALERT"nvramdisk: lv_table_page_alloc_error");
	
	
	
#if (defined NVRAMDISK_LVTABLE_UC) || (defined NVRAMDISK_LVTABLE_WT) || (defined NVRAMDISK_LVTABLE_WC)
	for(i=0; i <(1<<order); i++)
		SetPageReserved(page+i);	
	//lv_table = kzalloc(NUMOFSHADOW*4*sizeof(unsigned long), GFP_KERNEL);
	//nvdisk->nvdisk_manage->lv_tableold = page_address(page);	
#ifdef NVRAMDISK_LVTABLE_UC
	lv_table = ioremap_prot(page_to_phys(page), PAGE_SIZE<<order,_PAGE_CACHE_UC | _PAGE_RW);
#endif
#ifdef NVRAMDISK_LVTABLE_WT
	lv_table = ioremap_prot(page_to_phys(page), PAGE_SIZE<<order,_PAGE_CACHE_WT | _PAGE_RW);	
#endif
#ifdef NVRAMDISK_LVTABLE_WC
	lv_table = ioremap_prot(page_to_phys(page), PAGE_SIZE<<order,_PAGE_CACHE_WC | _PAGE_RW);	
#endif
	if(!lv_table)
		printk(KERN_ALERT"nvramdisk: lv_table ioremap error");
	nvdisk->nvdisk_manage->lv_table =  lv_table;
#else//WB, CLFLUSH, MOVNTQ
	lv_table = nvdisk->nvdisk_manage->lv_table = nvdisk->nvdisk_manage->lv_tableold;
#endif


	//lvtable initialization
	if(jmax<3)
	{
		printk(KERN_ALERT"jmax<3");		
	}
	
	
	for(n=0; n<NUMOFSHADOW; n++)
	{
		j = n & (~0x03);//n-(n%4); 
		jmax = j+4;
		i=n & 0x03;//%4;

		while(j<jmax)
		{
			lv_table[j][i]=0;
			j++;
		}
	}
	
	

}
static void inline lv_table_update(struct nvdisk_device *nvdisk, int i, int idx)
{
	int j,n, jmax;
	
#ifdef NVRAMDISK_LVTABLE_CLFLUSH	
	lvalue (*lv_table)[4];
	lv_table = nvdisk->nvdisk_manage->lv_table;
	j = i & (~0x03);//-(i%4);
	jmax = j+3;//j+4;
	i= i & 0x03;//%4;
	while(j<jmax)
	{
		//__asm__ ("movnti %1, %0" : "=m"(*&lv_table[j][i]): "r" (idx));
		lv_table[j][i]=idx;

		j++;
	}
	clflush_cache_range(&lv_table[j][i], cache_line_size());
	return;
	
#else
	lvalue (*lv_table)[3];
	lv_table = nvdisk->nvdisk_manage->lv_table;
#ifdef NVRAMDISK_LVTABLE_MOVNTQ
	j=0;
	jmax=3;	
	while(j<jmax)
	{

		__asm__  __volatile__(
			//"prefetchnta (%0)\n"
			"movq (%0), %%mm0\n"
			"movntq %%mm0, (%1)\n" 
			: :"r" (&idx), "r"(&lv_table[j][i]) : "memory");			
		j++;
	}


#else//NVRAMDISK_LVTABLE_UC || NVRAMDISK_LVTABLE_WT  || NVRAMDISK_LVTABLE_WC || NVRAMDISK_LVTABLE_WB
	lv_table[i][0]=idx;
	lv_table[i][1]=idx;
	lv_table[i][2]=idx;
#endif
#endif

#if (defined NVRAMDISK_LVTABLE_MOVNTQ) || (defined NVRAMDISK_LVTABLE_WC)
	__asm__ __volatile__("sfence");
#endif

return;
	
}
static void inline lv_table_clear(struct nvdisk_device *nvdisk, int i)
{
	int j,n, jmax;	
#ifdef NVRAMDISK_LVTABLE_CLFLUSH	
	lvalue (*lv_table)[4];
	lv_table = nvdisk->nvdisk_manage->lv_table;
	j = i & (~0x03);//-(i%4);
	jmax = j+3;//j+4;
	i= i & 0x03;//%4;
	while(j<jmax)
	{
		//__asm__ ("movnti %1, %0" : "=m"(*&lv_table[j][i]): "r" (idx));
		lv_table[j][i]=0;

		j++;
	}
	clflush_cache_range(&lv_table[j][i], cache_line_size());
	return;
	
#else
	n=0;
	lvalue (*lv_table)[3];
	lv_table = nvdisk->nvdisk_manage->lv_table;
#ifdef NVRAMDISK_LVTABLE_MOVNTQ
	j=0;
	jmax=3;
	while(j<jmax)
	{
		__asm__  __volatile__(
			//"prefetchnta (%0)\n"
			"movq (%0), %%mm0\n"
			"movntq %%mm0, (%1)\n" 
			: :"r" (&n), "r"(&lv_table[j][i]) : "memory");	
		j++;
	}
#else//NVRAMDISK_LVTABLE_UC || NVRAMDISK_LVTABLE_WT || NVRAMDISK_LVTABLE_CLFLUSH || || NVRAMDISK_LVTABLE_WC || NVRAMDISK_LVTABLE_WB
		//__asm__ __volatile__("movq (%0), (%1)"::"r"(&idx), ""(&lv_table[j][i]) : "memory");
	lv_table[i][0]=0;
	lv_table[i][1]=0;
	lv_table[i][2]=0;
#endif
#endif

#if (defined NVRAMDISK_LVTABLE_MOVNTQ) || (defined NVRAMDISK_LVTABLE_WC)
	__asm__ __volatile__("sfence");
#endif

return;

	
}
/*
 * Look up and return a brd's page for a given sector.
 */
static struct page *nvdisk_lookup_page(struct nvdisk_device *nvdisk, sector_t sector)
{	
	struct page *page;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	int idx = sector >> PAGE_SECTORS_SHIFT;
	
	nvramdisk_mapping_table_entry  *primary_table, *secondary_table, *shadow_table;
	
	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;

	page = virt_to_page(primary_table[idx]);	
	return page;
	
}

/*
 * Look up and return a nvdisk's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */

static void nvdisk_free_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	struct page *page;
	void* kaddr;			
	int idx;
	nvramdisk_mapping_table_entry  *primary_table, *secondary_table;
	void *value =0;
	
	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
	idx = sector >> PAGE_SECTORS_SHIFT;
#ifdef NVRAMDISK_DEBUG
//	printk(KERN_INFO"nvdisk: nvdisk_free_page()\n");
#endif
#ifdef LOCKING_ENABLE
	tslock(nvdisk, idx);
#endif 
	kaddr = primary_table[idx];
	if(kaddr)
	{
		if(kaddr != secondary_table[idx])
			printk(KERN_ALERT"nvdisk_free_page: error");
		nvdisk_log_update(&primary_table[idx], &value);
		nvdisk_table_update(&secondary_table[idx], &value);
		free_page(kaddr);
	}
	//clear_highpage(kaddr);
#ifdef LOCKING_ENABLE
	tsunlock(nvdisk,idx);
#endif
	
}

static void nvdisk_zero_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	//struct page *page;

	//page = nvdisk_lookup_page(nvdisk, sector);
	//if (page)
	//	clear_highpage(page);
	//struct page *page;
	void* kaddr;
	nvramdisk_mapping_table_entry  *primary_table, *secondary_table;
	//unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	int idx = sector >> PAGE_SECTORS_SHIFT;
	
	
	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
#ifdef LOCKING_ENABLE
	tslock(nvdisk, idx);
#endif
	//printk(KERN_ALERT"nvdisk_zero_page: DISCARD");
	if (primary_table[idx] != secondary_table[idx])
	{
		printk(KERN_ALERT"nvdisk_zero_page: error");
	}
	kaddr = primary_table[idx];
	if(kaddr)
	{
		clear_highpage(virt_to_page(kaddr));
		//memset(kaddr, 0, PAGE_SIZE);
	}	
	//primary_table[idx] = 0;
	//secondary_table[idx] =0;	
#ifdef LOCKING_ENABLE
	tsunlock(nvdisk,idx);	
#endif
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void nvdisk_free_pages(struct nvdisk_device *nvdisk)
{

	struct page *page;
	int i;
	unsigned long size, n;
	void* kaddr;		
	void* value = 0;
	nvramdisk_mapping_table_entry  *primary_table, *secondary_table;

#ifdef NVRAMDISK_DEBUG
	printk(KERN_INFO"nvdisk: nvdisk_free_pages()\n");
#endif
	size = nvdisk->nvdisk_manage->partition_size;
	n = size >> PAGE_SHIFT;
	
	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
	
	for(i=0; i<n; i++)
	{
		kaddr = primary_table[i];
		if (kaddr != secondary_table[i])
		{
			printk(KERN_ALERT"nvdisk_free_page: error mapping table missmatch");
			return;
		}		
		if(kaddr)
		{
#ifdef NVRAMDISK_DEBUG
//			printk(KERN_INFO"nvdisk: nvdisk_free_pages()-free_page()\n");
#endif		
			nvdisk_log_update(&primary_table[i], &value);
			nvdisk_table_update(&secondary_table[i], &value);

			free_page(kaddr);
		}
	}
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
		if (0)
			nvdisk_free_page(nvdisk, sector);
		else
			nvdisk_zero_page(nvdisk, sector);
		sector += PAGE_SIZE >> SECTOR_SHIFT;
		n -= PAGE_SIZE;
	}
}
/*struct log_entry
{
	int *lv1;
	int *lv2;
	int *lv3;
	void **mapping_table_entry;
	int *commit;
	void* block;
}
struct nvdisk_log
{
	int log_pos;
	int log_entries;
	spinlock_t log_lock;
}
struct nvdisk_log nvdisk_log;

void log_entry_init()
{
	struct log_entry* log_entry;
	int i;
	nvdisk_log.log_pos=0;
	nvdisk_log.log_entries=NUMOFSHADOW;
	log_entry=kmalloc(sizeof(struct log_entry)*NUMOFSHADOW);
	for(i=0; i<NUMOFSHADOW; i++)
	{
		log_entry[i].lv1=NULL;
		log_entry[i].lv2=NULL;
		log_entry[i].lv3=NULL;
		log_entry[i].mapping_table_entry=NULL;
		log_entry[i].commit=NULL;
		log_entry[i].block=NULL;
	}
	
}
static inline int log_entry_alloc()
{
	int ret=-1;
	if(nvdisk_log.log_entries > 32)
	{
		printk("nvramdisk: transaction start error.\n");
		return ret;
	}
	//spin_lock(&);	
	ret=nvdisk_log.log_pos++;
	nvdisk_log.log_entries++;
	//spin_unlock(&);
	return ret;

}
static inline int log_entry_free()
{
	spin_lock(&);
	nvdisk_log.log_entries--;
	spin_unlock(&);
}*/

static inline struct shadow_list* get_shadow(struct nvdisk_device *nvdisk)
{
	struct shadow_list *working_list;
	struct manage_map* map= nvdisk->nvdisk_manage;	
	unsigned int index;
	spin_lock(&map->list_lock);
	if(list_empty(&map->shadow_list))
	{
		printk(KERN_ALERT"nvdisk: shadow block is not available");
		spin_unlock(&map->list_lock);
		return 0;	
	}
	else
	{
		working_list = list_first_entry(&map->shadow_list, struct shadow_list, list);
		//index=working_list->index;		
		list_del(&working_list->list);
		spin_unlock(&map->list_lock);
		return working_list;
	}

}
static inline void free_shadow(struct nvdisk_device *nvdisk, struct shadow_list * working_list)
{
	struct manage_map* map= nvdisk->nvdisk_manage;	
	spin_lock(&map->list_lock);
	list_add(&working_list->list, &map->shadow_list);
	spin_unlock(&map->list_lock);
}
static inline void nvdisk_block_write(void *dst, void *src, void *tmp, int offset, int copy)
{
#ifdef NVRAMDISK_BLOCK_MOVNTQ
	if(offset)
		memcpy_arjanv_movntq(dst, tmp, offset);			
	memcpy_arjanv_movntq(dst + offset, src, copy);
	if(copy+offset < PAGE_SIZE)
		memcpy_arjanv_movntq(dst + offset + copy, tmp+offset+copy, PAGE_SIZE-(copy+offset));
	
		
#else //NVRAMDISK_BLOCK_CLFLUSH ||NVRAMDISK_BLOCK_WT || NVRAMDISK_BLOCK_WB || NVRAMDISK_BLOCK_WC || NVRAMDISK_BLOCK_UC
	if(offset)
		memcpy(dst, tmp, offset);	
	memcpy(dst + offset, src, copy);
	if(copy+offset < PAGE_SIZE)
		memcpy(dst + offset + copy, tmp+offset+copy, PAGE_SIZE-(copy+offset));
#endif

#ifdef NVRAMDISK_BLOCK_CLFLUSH
	clflush_cache_range(dst, PAGE_SIZE);
#endif

#if (defined NVRAMDISK_BLOCK_MOVNTQ) || (defined NVRAMDISK_BLOCK_WC)
	__asm__ __volatile__("mfence");
#else //NVRAMDISK_BLOCK_WT || NVRAMDISK_BLOCK_UC
	;
#endif
}
/*
 * Copy n bytes from src to the nvdisk starting at sector. Does not sleep.
 */
static void copy_to_nvdisk(struct nvdisk_device *nvdisk, const void *src,
			sector_t sector, size_t n)
{
	struct shadow_list *working_list;
	unsigned int index;
	struct manage_map* map= nvdisk->nvdisk_manage;	
	
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	struct page* page;
	void * start_addr = nvdisk->nvdisk_manage->start;
	void * dst;
	struct page * new_page;
	//unsigned long addr;
		
	size_t copy;
	
	int idx = sector >> PAGE_SECTORS_SHIFT;
	nvramdisk_mapping_table_entry tmp;
	nvramdisk_mapping_table_entry *primary_table, *secondary_table, *shadow_table;
	
	
#ifdef NVRAMDISK_TIMECHECK	
	struct timespec p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12;
	unsigned int pt1, pt2,pt3,pt4,pt5,pt6,pt7,pt8,pt9,pt10,pt11,pt12;
	unsigned int talloc_page, tfree_page, tlbflush, total, metadata_logging, shadow_block, etc, memcpy;
	unsigned int N_SEC = 1000000000;
#endif
#ifndef NVRAMDISK_TIMECHECK	
	copy = min_t(size_t, n, PAGE_SIZE - offset);

	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
	shadow_table = nvdisk->nvdisk_manage->tables;
	
	tmp=secondary_table[idx];
	if(!tmp)
	{
		printk(KERN_ALERT"BUG: NULL tmp");
		new_page = alloc_page(GFP_ATOMIC | __GFP_ZERO);//kmap_atomic(new_page, KM_USER1);		
		tmp=__va(PFN_PHYS(page_to_pfn(new_page)));//page_address(new_page);		
	}
	working_list = get_shadow(nvdisk);
	if(!working_list)
		printk(KERN_ALERT"nvdisk: get_shadow returen 0");
	index = working_list->index;
#ifdef SHADOW_BLOCK_ENABLE
	dst = shadow_table[index];
#else
	page = alloc_page(GFP_ATOMIC);
	dst = page_address(page);	
#endif
	
	
	nvdisk_block_write(dst, src, tmp, offset, copy);	
	lv_table_update(nvdisk, index, idx);
#ifdef LOCKING_ENABLE
	tslock(nvdisk, idx);
#endif
	nvdisk_log_update(&primary_table[idx], &dst);
	nvdisk_table_update(&secondary_table[idx], &dst);	
#ifdef LOCKING_ENABLE
	tsunlock(nvdisk, idx);	
#endif
	shadow_table[index] = tmp;
	lv_table_clear(nvdisk, index);
	free_shadow(nvdisk, working_list);	
#ifndef SHADOW_BLOCK_ENABLE
	free_page(tmp);
#endif






#else//TIMECHECK
	getnstimeofday(&p9);
	copy = min_t(size_t, n, PAGE_SIZE - offset);

	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
	shadow_table = nvdisk->nvdisk_manage->tables;
	
	getnstimeofday(&p1);
	tmp=secondary_table[idx];	
	if(!tmp)
		printk(KERN_ALERT"BUG: NULL tmp");
	
	working_list = get_shadow(nvdisk);
	if(!working_list)
		printk(KERN_ALERT"nvdisk: get_shadow returen 0");
	index = working_list->index;	
#ifndef SHADOW_BLOCK_ENABLE
	page = alloc_page(GFP_ATOMIC);
#endif
	
#ifdef SHADOW_BLOCK_ENABLE
	dst = shadow_table[index];
#else	
	dst = page_address(page);	
#endif
	getnstimeofday(&p2); 
	
	nvdisk_block_write(dst, src, tmp, offset, copy);	
	getnstimeofday(&p3);
	
	lv_table_update(nvdisk, index, idx);	
	
//#ifdef LOCKING_ENABLE
//	tslock(nvdisk, idx);
//	getnstimeofday(&p7);
//#else
//	p7=p6;
//#endif	
	nvdisk_log_update(&primary_table[idx], &dst);
	getnstimeofday(&p4);
	
		
	nvdisk_table_update(&secondary_table[idx], &dst);
	getnstimeofday(&p5);
	
	
	shadow_table[index] = tmp;
	getnstimeofday(&p6);
	
	
	lv_table_clear(nvdisk, index);
	getnstimeofday(&p7);
	
	
//#ifdef LOCKING_ENABLE
//	tsunlock(nvdisk, idx);	
//#endif

	free_shadow(nvdisk, working_list);
	getnstimeofday(&p8);

	//page=alloc_page(GFP_ATOMIC);
	//getnstimeofday(&p9);
	//__free_page(page);
	//getnstimeofday(&p10);
	//__native_flush_tlb_single(dst);
	//getnstimeofday(&p11);
#ifndef SHADOW_BLOCK_ENABLE
	free_page(tmp);	
#endif
	//getnstimeofday(&p12);
#endif
	
#ifdef NVRAMDISK_TIMECHECK	
	pt1=p1.tv_sec*N_SEC+p1.tv_nsec;
	pt2=p2.tv_sec*N_SEC+p2.tv_nsec;
	pt3=p3.tv_sec*N_SEC+p3.tv_nsec;
	pt4=p4.tv_sec*N_SEC+p4.tv_nsec;
	pt5=p5.tv_sec*N_SEC+p5.tv_nsec;
	pt6=p6.tv_sec*N_SEC+p6.tv_nsec;
	pt7=p7.tv_sec*N_SEC+p7.tv_nsec;
	pt8=p8.tv_sec*N_SEC+p8.tv_nsec;
	pt9=p9.tv_sec*N_SEC+p9.tv_nsec;
	pt10=p10.tv_sec*N_SEC+p10.tv_nsec;
	pt11=p11.tv_sec*N_SEC+p11.tv_nsec;
	pt12=p12.tv_sec*N_SEC+p12.tv_nsec;

	total= pt8-pt9;
	shadow_block = (pt2-pt1)+ (pt6-pt5) +(pt8-pt7) + (pt5-pt4);
	memcpy=pt3-pt2;
	metadata_logging = (pt4-pt3) +(pt7-pt6);
	talloc_page = pt9-pt8;
	tfree_page = pt10-pt9;
	tlbflush=pt11-pt10;
	etc = pt1 - pt9;
	//lookup, getshadow+freeshadow, lock+unlock, lvupdate+lvclear, tableupdate, memcpy, etc
	printk(KERN_INFO"%d %d %d %d %d %d %d\n", total, shadow_block, metadata_logging, memcpy, etc, tfree_page, tlbflush);
	//printk(KERN_ALERT"%dB total:%d update:%d shadow:%d memcpy:%d flush:%d lvalue:%d flock:%d \n", n, total2-total1, pgtable_update2-pgtable_update1, (shadow2-shadow1) + (lock2-lock1), memcpy2-memcpy1, data_flush2-data_flush1, (lv4-lv3) + (lv2-lv1),( flock4-flock3 )+ (flock2 -flock1) );
#endif
	//printk(KERN_ALERT"-----------AFTER-------------");	
	//printk(KERN_ALERT"des_addr=%x", des);	
	//printk(KERN_ALERT"=%x", __va(PFN_PHYS(page_to_pfn(page_dest))));
	//printk(KERN_ALERT"shadow_addr=%x", shadow_addr);
	//printk(KERN_ALERT"dst=%x", __va(PFN_PHYS(page_to_pfn(page_shadow))));
	//up(&shadow_sem);

	
	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		idx = sector >> PAGE_SECTORS_SHIFT;
#ifdef NVRAMDISK_DEBUG
		printk(KERN_INFO"nvdisk: copy_to_nvdisk() while copy<n\n");
#endif
		tmp=primary_table[idx];
		if(!tmp)
		{
			printk(KERN_ALERT"BUG: NULL tmp");
			new_page = alloc_page(GFP_ATOMIC | __GFP_ZERO);//kmap_atomic(new_page, KM_USER1);		
			tmp=__va(PFN_PHYS(page_to_pfn(new_page)));//page_address(new_page);		
		}
		working_list = get_shadow(nvdisk);
		if(!working_list)
			printk(KERN_ALERT"nvdisk: get_shadow returen 0");
		index = working_list->index;
		
		dst = shadow_table[index];
		nvdisk_block_write(dst, src, tmp, 0, copy);	
		lv_table_update(nvdisk, index, idx);	
#ifdef LOCKING_ENABLE
		tslock(nvdisk, idx);
#endif
		nvdisk_log_update(&primary_table[idx], &dst);
		nvdisk_table_update(&secondary_table[idx], &dst);
#ifdef LOCKING_ENABLE
		tsunlock(nvdisk, idx);	
#endif
		shadow_table[index] = tmp;
		lv_table_clear(nvdisk, index);
		free_shadow(nvdisk, working_list);			
	}
}

/*
 * Copy n bytes to dst from the nvdisk starting at sector. Does not sleep.
 */
static void copy_from_nvdisk(void *dst, struct nvdisk_device *nvdisk,
			sector_t sector, size_t n)
{	
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	int idx = sector >> PAGE_SECTORS_SHIFT;
	size_t copy;
	nvramdisk_mapping_table_entry  *primary_table, *secondary_table, *shadow_table;
	
	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
		
	copy = min_t(size_t, n, PAGE_SIZE - offset);
#ifdef LOCKING_ENABLE
	tslock(nvdisk, idx);
#endif
	src = secondary_table[idx];
#ifdef LOCKING_ENABLE
	tsunlock(nvdisk, idx);
#endif
	if(src)
		memcpy(dst, src+offset, copy);
	else
		memset(dst, 0, copy);
	
	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		idx = sector >> PAGE_SECTORS_SHIFT;
#ifdef LOCKING_ENABLE
		tslock(nvdisk, idx);
#endif
		src = secondary_table[idx];
#ifdef LOCKING_ENABLE
		tsunlock(nvdisk, idx);
#endif
		if(src)
			memcpy(dst, src, copy);
		else
			memset(dst, 0, copy);
	}

}
static struct page *nvdisk_insert_page(struct nvdisk_device *nvdisk, sector_t sector)
{
	nvramdisk_mapping_table_entry  *primary_table, *secondary_table;
	struct manage_map* map= nvdisk->nvdisk_manage;
	struct page * page=0;
	nvramdisk_mapping_table_entry tmp;
	pgoff_t  idx;
	
	idx = sector >> PAGE_SECTORS_SHIFT;

	primary_table = nvdisk->nvdisk_manage->table1;
	secondary_table = nvdisk->nvdisk_manage->table2;
	
	//unsigned long addr;
	tmp=secondary_table[idx];
	if(tmp)
		return virt_to_page(tmp);
#ifdef LOCKING_ENABLE
	tslock(nvdisk, idx);
#endif
	page = alloc_page(GFP_NOIO | __GFP_ZERO);//kmap_atomic(page, KM_USER1);	
	if(!page)
		return NULL;//page;
	else
		page->index = idx;
	tmp=page_address(page);//__va(PFN_PHYS(page_to_pfn(page)));
	nvdisk_log_update(&primary_table[idx], &tmp);
	nvdisk_table_update(&secondary_table[idx], &tmp);
	//mb();
#ifdef LOCKING_ENABLE
	tsunlock(nvdisk, idx);
#endif
	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support XIP and highmem, because our ->direct_access
	 * routine for XIP must return memory that is always addressable.
	 * If XIP was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */	
	return page;//page;
}
static int copy_to_nvdisk_setup(struct nvdisk_device *nvdisk, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!nvdisk_insert_page(nvdisk, sector))
		return -ENOMEM;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!nvdisk_insert_page(nvdisk, sector))
			return -ENOMEM;
	}
	return 0;	
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
	
	if (rw != READ) {
		err = copy_to_nvdisk_setup(nvdisk, sector, len);
		if (err)
			goto out;
	}
	
	mem = kmap_atomic(page, KM_USER0);
	if (rw == READ) {
		copy_from_nvdisk(mem + off, nvdisk, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_nvdisk(nvdisk, mem + off, sector, len);
	}
	kunmap_atomic(mem, KM_USER0);
	udelay(29);	

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
	int idx=sector >> PAGE_SECTORS_SHIFT;;
#ifdef NVRAMDISK_DEBUG
	printk(KERN_INFO"nvdisk: nvdisk_direct_access()\n");
#endif

	if (!nvdisk)
		return -ENODEV;
	if (sector & (PAGE_SECTORS-1))
		return -EINVAL;
	if (sector + PAGE_SECTORS > get_capacity(bdev->bd_disk))
		return -ERANGE;
	//page = nvdisk_insert_page(nvdisk, sector);
	idx = sector >> PAGE_SECTORS_SHIFT;
	if(!nvdisk->nvdisk_manage->table1[idx])
	{
		page = alloc_page(GFP_NOIO | __GFP_ZERO);
		if (!page)
		return -ENOMEM;
		nvdisk->nvdisk_manage->table1[idx] = nvdisk->nvdisk_manage->table2[idx] = __va(PFN_PHYS(page_to_pfn(page)));//page_address(page);
		*kaddr = __va(PFN_PHYS(page_to_pfn(page)));//page_address(page);
		*pfn = page_to_pfn(page);
	}
	else
	{
		*kaddr = nvdisk->nvdisk_manage->table1[idx];
		*pfn = virt_to_page(nvdisk->nvdisk_manage->table1[idx]);
	}
	

	

	return 0;
}
#endif

static int nvdisk_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	int error;
	struct nvdisk_device *nvdisk = bdev->bd_disk->private_data;
#ifdef NVRAMDISK_DEBUG
	printk(KERN_INFO"nvdisk: nvdisk_ioctl()\n");
#endif
	return -ENOTTY;





	if (cmd != BLKFLSBUF)
		return -ENOTTY;

	/*
	 * ram device BLKFLSBUF has special semantics, we want to actually
	 * release and destroy the ramdisk data.
	 */
	//lock_kernel();
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
	//unlock_kernel();

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
int nvrd_size = 10*1024*1024*2;//CONFIG_BLK_DEV_RAM_SIZE;
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


int nvramdisk_mapping_table_init(struct nvdisk_device* nvdisk){
	int size = nvrd_size*512;
	int entry_count = size / PAGE_SIZE;//sizeof(nvramdisk_mapping_table_entry);
	int order;
	struct page *page;
	struct page * t1_pages;
	struct page * t2_pages;
	struct page * ts_pages;
	cycles_t c1, c2, c3;
	int i;

	unsigned int level;
	pte_t *pte;	
	
	order = number_of_bits(entry_count * sizeof(nvramdisk_mapping_table_entry) / PAGE_SIZE);
	order = number_of_bits(8192);
		
	
	/*Primary Mapping Table Allocation*/
	if(nvdisk->nvdisk_manage->table1oldmap)
	{
		page=virt_to_page((void*)nvdisk->nvdisk_manage->table1oldmap);
		nvdisk->nvdisk_manage->table1=page_address(page);
		if(nvdisk->nvdisk_manage->table1oldmap  != page_address(page))
			printk(KERN_ALERT"nvramdisk: why different?\n");
		printk(KERN_ALERT"nvramdisk: fixed allocation primary mapping table\n");	
		
	}
	else
	{
		page = alloc_pages(GFP_KERNEL|__GFP_ZERO, order);	
		nvdisk->nvdisk_manage->table1=page_address(page);
		nvdisk->nvdisk_manage->table1oldmap = nvdisk->nvdisk_manage->table1;	
	}
	if(!page)
		printk(KERN_ALERT"nvramdisk: primary mapping table allocation error");	
#if (defined NVRAMDISK_LVTABLE_WT) || (defined NVRAMDISK_LVTABLE_UC) || (defined NVRAMDISK_LVTABLE_WC)
	for(i=0; i <(1<<order); i++)
		SetPageReserved(page+i);		
	
	
#ifdef NVRAMDISK_LVTABLE_WT
	nvdisk->nvdisk_manage->table1=(void**)ioremap_prot(page_to_phys(page), PAGE_SIZE<<order,_PAGE_CACHE_WT| _PAGE_RW);
#endif

#ifdef NVRAMDISK_LVTABLE_UC
	nvdisk->nvdisk_manage->table1=(void**)ioremap_prot(page_to_phys(page), PAGE_SIZE<<order,_PAGE_CACHE_UC| _PAGE_RW);
#endif

#ifdef NVRAMDISK_LVTABLE_WC
	nvdisk->nvdisk_manage->table1=(void**)ioremap_prot(page_to_phys(page), PAGE_SIZE<<order,_PAGE_CACHE_WC| _PAGE_RW);
#endif 
	if(!nvdisk->nvdisk_manage->table1)
		printk(KERN_ALERT"nvramdisk: primary mapping table ioremap error");
#endif
	
	

	/*Secondary Mapping Table Allocation*/
	if(nvdisk->nvdisk_manage->table2oldmap)
	{	
		page=virt_to_page((void*)nvdisk->nvdisk_manage->table2oldmap);		
		nvdisk->nvdisk_manage->table2=page_address(page);
	}
	else
	{
		page = alloc_pages(GFP_KERNEL|__GFP_ZERO, order);
		nvdisk->nvdisk_manage->table2=page_address(page);
		nvdisk->nvdisk_manage->table2oldmap = nvdisk->nvdisk_manage->table2;	
	}
	if(!page)		
		printk(KERN_ALERT"nvramdisk: secondary mapping table allocation error");	
#if (defined NVRAMDISK_MAPPING_TABLE_WT) || (defined NVRAMDISK_MAPPING_TABLE_UC) || (defined NVRAMDISK_MAPPING_TABLE_WC)
	for(i=0; i <(1<<order); i++)
		SetPageReserved(page+i);	
#ifdef NVRAMDISK_MAPPING_TABLE_WT
	nvdisk->nvdisk_manage->table2=(void*)ioremap_prot(page_to_phys(page), PAGE_SIZE<<order, _PAGE_CACHE_WT| _PAGE_RW);
#endif
#ifdef NVRAMDISK_MAPPING_TABLE_UC
	nvdisk->nvdisk_manage->table2=(void*)ioremap_prot(page_to_phys(page), PAGE_SIZE<<order, _PAGE_CACHE_UC| _PAGE_RW);
#endif
#ifdef NVRAMDISK_MAPPING_TABLE_WC
	nvdisk->nvdisk_manage->table2=(void*)ioremap_prot(page_to_phys(page), PAGE_SIZE<<order, _PAGE_CACHE_WC| _PAGE_RW);
#endif 	


#endif
	/*Shadow Mapping Table Allocation*/
	nvdisk->nvdisk_manage->tables=kzalloc(NUMOFSHADOW*sizeof(nvramdisk_mapping_table_entry), GFP_KERNEL);
	if(!nvdisk->nvdisk_manage->tables)
		printk(KERN_ALERT"nvramdisk: shadow mapping table allocation error");	

	/*
	for(i=0; i< (1<<order) ; i++)
	{
		set_kernel_page_prot(virt_to_page( ((void*)nvdisk->nvdisk_manage->table1)+PAGE_SIZE*i ),   __pgprot(_PAGE_PWT));
		set_kernel_page_prot(virt_to_page( ((void*)nvdisk->nvdisk_manage->table2)+PAGE_SIZE*i ),   __pgprot(_PAGE_PWT));
	}
	set_kernel_page_prot(virt_to_page( ((void*)nvdisk->nvdisk_manage->tables)),  __pgprot(_PAGE_PWT));
	*/
	//add error handler
	return 0;
}
void nvramdisk_mapping_table_destroy(struct nvdisk_device* nvdisk){
	/*kfree(nvdisk->nvdisk_manage->table1);
	kfree(nvdisk->nvdisk_manage->table2);
	kfree(nvdisk->nvdisk_manage->tables);
	*/
	//add error handler	
	int size = nvrd_size*512;
	int entry_count = size / PAGE_SIZE;//sizeof(nvramdisk_mapping_table_entry);
	int order;
	struct page* page;
	int i;
	order = number_of_bits(entry_count * sizeof(nvramdisk_mapping_table_entry) / PAGE_SIZE);	
#if (defined NVRAMDISK_MAPPING_TABLE_UC) || (defined NVRAMDISK_MAPPING_TABLE_WT) || (defined NVRAMDISK_MAPPING_TABLE_WC)
	page = virt_to_page(nvdisk->nvdisk_manage->table1oldmap);
	for(i=0; i <(1<<order); i++)
		ClearPageReserved(page+i);
	page = virt_to_page(nvdisk->nvdisk_manage->table2oldmap);
	for(i=0; i <(1<<order); i++)
		ClearPageReserved(page+i);	
	iounmap(nvdisk->nvdisk_manage->table1);
	iounmap(nvdisk->nvdisk_manage->table2);
#endif//NVRAMDISK_MAPPING_TABLE_WB || NVRAMDISK_MAPPING_TABLE_MOVNTQ || NVRAMDISK_MAPPING_TABLE_CLFLUSH
	free_pages(nvdisk->nvdisk_manage->table1oldmap, order);
	free_pages(nvdisk->nvdisk_manage->table2oldmap, order);	

	kfree(nvdisk->nvdisk_manage->tables);

	/*
	for(i=0; i< (1<<order) ; i++)
	{
		set_kernel_page_prot(virt_to_page( ((void*)nvdisk->nvdisk_manage->table1)+PAGE_SIZE*i ), __pgprot(0));
		set_kernel_page_prot(virt_to_page( ((void*)nvdisk->nvdisk_manage->table2)+PAGE_SIZE*i ),  __pgprot(0));
	}
	set_kernel_page_prot(virt_to_page( ((void*)nvdisk->nvdisk_manage->tables)),__pgprot(0));
	*/
}
#ifdef NVRAMDISK_FINE_GRAINED_LOCKING
int subset_lock_init(struct nvdisk_device* nvdisk){
	//lock √ ±‚»≠ size / subset
	int n;
	int i;
	//int size;
	spinlock_t *tslocks;
	//n = ((nvrd_size << SECTOR_SHIFT) / PAGE_SIZE) / (cache_line_size()/sizeof(nvramdisk_mapping_table_entry));
	n = ((nvrd_size << SECTOR_SHIFT) / PAGE_SIZE);
	tslocks=(spinlock_t *)kzalloc(sizeof(spinlock_t) * n, GFP_KERNEL);
	//tslocks = (spinlock_t*)vmalloc(sizeof(spinlock_t) * n);
	for(i=0; i<n; i++)
	{
		spin_lock_init(&tslocks[i]);
	}	
	nvdisk->nvdisk_manage->table_subset_locks = tslocks;	
	return 0;
}
void subset_lock_destroy(struct nvdisk_device* nvdisk){
	//vfree(nvdisk->nvdisk_manage->table_subset_locks);
	kfree(nvdisk->nvdisk_manage->table_subset_locks);	
}
#endif

static struct nvdisk_device *nvdisk_alloc(int i)
{
	struct page* page;
	struct nvdisk_device *nvdisk;
	struct gendisk *disk;
	
	struct manage_map* nvdisk_manage;
	void * shadow_start;
	struct page* shadow_page;
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
	//blk_queue_ordered(nvdisk->nvdisk_queue, QUEUE_ORDERED_TAG);
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


	//page = alloc_page(GFP_KERNEL);

	page = pfn_to_page(NVRAMDISK_START_PFN);


	nvdisk_manage = page_address(page);
	//page->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	//page->flags &= ~PAGE_FLAGS_CHECK_AT_FREE;
	//SetPageReserved(page);
	//nvdisk_manage=(void*)ioremap_prot(page_to_phys(page), PAGE_SIZE, _PAGE_CACHE_WT| _PAGE_RW);
	

	printk(KERN_ALERT"NVRAMDISK: pfn=%x\n", NVRAMDISK_START_PFN);
	printk(KERN_ALERT"NVRAMDISK: page=%x\n", pfn_to_page(NVRAMDISK_START_PFN));
	printk(KERN_ALERT"NVRAMDISK: vaddr=%x\n", (void*)nvdisk_manage);

	if(!nvdisk_manage)
		printk(KERN_ALERT"nvdisk: manage_map alloc error");	
	//printk(KERN_INFO"NVRAMDISK: page\n");
	//nvdisk_manage = page_address(alloc_page(GFP_KERNEL));//page_address(pfn_to_page(NVRAMDISK_START_PFN));
	if(!(nvdisk_manage->magic == 0x12345678))
	{
		nvdisk_manage->magic = 0x12345678;		
	}	
	//kzalloc(sizeof(struct manage_map), GFP_KERNEL);
	
	nvdisk->nvdisk_manage = nvdisk_manage;
	nvdisk->nvdisk_manage->partition_size = nvrd_size<<SECTOR_SHIFT;
	nvdisk->nvdisk_manage->num_blocks = (nvrd_size<<SECTOR_SHIFT)>>PAGE_SHIFT;
	//nvdisk->nvdisk_manage->start = vmalloc(nvrd_size*512);
	//nvdisk->nvdisk_manage->table1=alloc_page(GFP_KERNEL);
	//nvdisk->nvdisk_manage->table2=alloc_page(GFP_KERNEL);

	//nvdisk->nvdisk_manage->table1_mm->pgd = alloc_pg
	//t1=nvdisk->nvdisk_manage->table1;
	//t2=nvdisk->nvdisk_manage->table2;
	
	//printk(KERN_ALERT"nvdisk: partition alloc %x", nvdisk->nvdisk_manage->start);
	//nvdisk->nvdisk_manage->	shadow_start = vmalloc(PAGE_SIZE * NUMOFSHADOW);
	//printk(KERN_ALERT"nvdisk: shadow block alloc %x", nvdisk->nvdisk_manage->shadow_start);
	printk(KERN_ALERT"nvdisk: list init");
	INIT_LIST_HEAD(&nvdisk_manage->shadow_list);
	printk(KERN_ALERT"nvdisk: list for init");
	nvramdisk_mapping_table_init(nvdisk);
	printk(KERN_ALERT"nvdisk: mapping table init");
	for(i=0; i<NUMOFSHADOW; i++)
	{
		struct shadow_list * tmp_list;
		//void* shadow_start = nvdisk->nvdisk_manage->	shadow_start;
		tmp_list = (struct shadow_list*)kmalloc(sizeof(struct shadow_list), GFP_KERNEL);
		tmp_list->index=i;
		list_add( &tmp_list->list, &nvdisk_manage->shadow_list);
		shadow_page = alloc_page(GFP_NOIO | __GFP_ZERO);
		if(!shadow_page)
			printk(KERN_ALERT"nvramdisk: shadow Block allocation error\n");
		nvdisk_manage->tables[i] = page_address(shadow_page);
		//shadow[i] = shadow_start + PAGE_SIZE * i;
		//printk("Shadow Block %d in %llX\n", i, nvdisk->nvdisk_shadow[i]);
		//nvdisk_manage->set[i].l1 = 0;
		//nvdisk_manage->set[i].l2 = 0;		
	}
	printk(KERN_ALERT"nvdisk: spinlock init");
	lv_table_init(nvdisk);
	printk(KERN_ALERT"nvdisk: location value table initialization");
#ifdef NVRAMDISK_FINE_GRAINED_LOCKING
	subset_lock_init(nvdisk);
	printk(KERN_ALERT"nvdisk: Fine-grained locking initialization");
#else
	spin_lock_init(&nvdisk->nvdisk_manage->nvramdisk_lock);
#endif
	
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
	int i;
	struct shadow_list * next_list;
	struct shadow_list * cur_list;
	put_disk(nvdisk->nvdisk_disk);
	blk_cleanup_queue(nvdisk->nvdisk_queue);

	
	for(i=0; i<NUMOFSHADOW; i++)
	{
		free_page(nvdisk->nvdisk_manage->tables[i]);		
	}
	printk(KERN_INFO "nvdisk: shadow block free\n");
	list_for_each_entry_safe(cur_list, next_list, &nvdisk->nvdisk_manage->shadow_list,  list) {		
		list_del(&cur_list->list);
		kfree(cur_list);
	}
	printk(KERN_INFO "nvdisk: list free\n");
	lv_table_destory(nvdisk);
	printk(KERN_INFO "nvdisk: lv_table_destroy\n");
#ifdef NVRAMDISK_FINE_GRAINED_LOCKING
	subset_lock_destroy(nvdisk);
	printk(KERN_INFO "nvdisk: subset lock destory\n");
#endif
	nvdisk_free_pages(nvdisk);
	printk(KERN_INFO "nvdisk: page free\n");
	nvramdisk_mapping_table_destroy(nvdisk);
	printk(KERN_INFO "nvdisk: mapping table free\n");
	kfree(nvdisk->nvdisk_manage);
	printk(KERN_INFO "nvdisk: nvram manage free\n");
	kfree(nvdisk);
	printk(KERN_INFO "nvdisk: nvdisk free\n");
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
#ifdef NVRAMDISK_TIMECHECK
	printk(KERN_INFO"lookup  shadow lock lvtable tableupdate memcpy etc");
#endif
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

