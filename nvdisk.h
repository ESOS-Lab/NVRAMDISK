#define NVRAMDISK_START_PFN 0x00300000//0x000000010f000000 >> PAGE_SHIFT
#define NVRAMDISK_MAP_SIZE 0x0000
#define NVRAMDISK_T1_PFN NVRAMDISK_START_PFN + 0x400
#define NVRAMDISK_T2_PFN NVRAMDISK_T1_PFN + 0x2000
#define NVRAMDISK_LV_PFN NVRAMDISK_T2_PFN + 0x2000




#define NVRAMDISK_SHADOW_BLOCK
#define NVRAMDISK_MEMCPY
#define NVRAMDISK_METADATA_CLFLUSH
//#define LOCKING_ENABLE
//#define NVRAMDISK_FINE_GRAINED_LOCKING


#define NVRAMDISK_MAJOR		262
#define NUMOFSHADOW 32

typedef unsigned long lvalue;


typedef void* nvramdisk_mapping_table_entry;


/*unsigned long table_update(unsigned long *mt1, unsigned long idx, unsigned long addr)
{
	// 3 level mapping table 10 10 10
	int n = PAGE_SIZE / sizeof(unsigned long);

	int idx1, idx2, idx3;
	unsigned long * mt2, *mt3, entry;
	struct page* page1, page2, page3;
	
	//overflow test

	idx1 = idx / n*n * PAGE_SIZE;
	idx2 = idx / n*PAGE_SIZE;
	idx3 = idx / PAGE_SIZE;

	mt2 = mt1[idx1];
	if(!mt1[idx1])
	{
		page1 = alloc_page(GFP_KERNEL);
		mt1_lock();
		mt2 = page1;
	}
	
	mt3 = mt2[idx2];
	if(!mt2[idx2])
	{
		page2 = alloc_page(GFP_KERNEL);
		mt2_lock();
		mt2[idx2] = page2;
	}
	mt3 = mt2[idx2];
	if(!mt3[idx3])
	{
		page3 = alloc_page(GFP_KERNEL);
		mt3[idx3] = page_address(page3);
	}
	
	mt3[idx3] = addr;
	
	mt1[idx1] = page_address(page1);
	mt1_unlock();
	mt2_unlock();
	mt3_unlock();
	return entry;
	
	
	
}*/
//typedef unsigned long *[4];
struct manage_map
{
	unsigned long long magic;
	unsigned long long block_size;
	unsigned long long num_blocks;
	void* shadow_start, *shadow_end;
	void* start, *end;
	unsigned long partition_size;
	//void* shadow[NUMOFSHADOW];	
	//shadow_variables set[NUMOFSHADOW];
	nvramdisk_mapping_table_entry* table1;
	nvramdisk_mapping_table_entry* table2;	
	nvramdisk_mapping_table_entry *tables;//volatile
	
	nvramdisk_mapping_table_entry* table1oldmap;//volatile
	nvramdisk_mapping_table_entry* table2oldmap;//volatile
	

#ifdef NVRAMDISK_FINE_GRAINED_LOCKING
	spinlock_t *table_subset_locks;//volatile
#else
	spinlock_t nvramdisk_lock;
#endif
	//struct mm_struct table1_mm;
	//struct mm_struct table2_mm;
	struct list_head shadow_list;//volatile
	spinlock_t list_lock;	//volatile
	lvalue **lv_tableold;//volatile
	lvalue (*lv_table)[4];
	
};






