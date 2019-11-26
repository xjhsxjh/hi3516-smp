/******************************************************************************

    Copyright (C), 2017, Hisilicon Tech. Co., Ltd.

******************************************************************************
  File Name     : ext_aead.c
  Version       : Initial Draft
  Created       : 2017
  Last Modified :
  Description   :
  Function List :
  History       :
******************************************************************************/
#include "drv_osal_lib.h"
#include <linux/dmapool.h>
#include <asm/cacheflush.h>

/************************* Internal Structure Definition *********************/
/** \addtogroup      base type*/
/** @{*/  /** <!-- [base]*/


/* under TEE, we only can malloc secure mmz at system steup,
 * then map the mmz to Smmu, but the smmu can't map to cpu address,
 * so we must save the cpu address in a static table when malloc and map mmz.
 * when call crypto_mem_map, we try to query the table to get cpu address firstly,
 * if can't get cpu address from the table, then call system api to map it.
 */
#define CRYPTO_MEM_MAP_TABLE_DEPTH      32
#define DMA_ALLOC_MAX_SIZE              (1024 * 256)

typedef struct
{
    u32 valid;
    compat_addr dma;
    void       *via;
}crypto_mem_map_table;

static crypto_mem_map_table loacl_map_table[CRYPTO_MEM_MAP_TABLE_DEPTH];

#ifdef CONFIG_64BIT
#define crypto_flush_dcache_area        __flush_dcache_area
#else
#define crypto_flush_dcache_area        __cpuc_flush_dcache_area
#endif

/** @}*/  /** <!-- ==== Structure Definition end ====*/

/******************************* API Code *****************************/
/** \addtogroup      base*/
/** @{*/  /** <!--[base]*/

/*****************************************************************
 *                       mmz/mmu api                             *
 *****************************************************************/
static s32 cipher_dma_set_mask(struct device *dev)
{
    s32 ret = HI_FAILURE;

    ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(64));
    if (HI_SUCCESS == ret)
    {
        return ret;
    }

    ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
    if (HI_SUCCESS != ret)
    {
        HI_LOG_ERROR("Failed to set DMA mask %d.\n", ret);
        return ret;
    }

    return HI_SUCCESS;
}

static s32  cipher_dma_alloc_coherent(crypto_mem *mem, u32 type, char const *name, u32 size)
{
    struct device *dev = HI_NULL;
    s32 ret = HI_FAILURE;
    s32 i;

    if (HI_NULL == mem)
    {
        HI_LOG_ERROR("mem is null.\n");
        return HI_FAILURE;
    }

    if (DMA_ALLOC_MAX_SIZE <= size)
    {
        HI_LOG_ERROR("dma alloc coherent with invalid size(0x%x).\n", size);
        return HI_FAILURE;
    }

    dev = (struct device *)cipher_get_device();
    if (HI_NULL == dev)
    {
        HI_LOG_ERROR("cipher_get_device error.\n");
        return HI_FAILURE;
    }

    ret = cipher_dma_set_mask(dev);
    if (HI_SUCCESS != ret)
    {
        HI_LOG_ERROR("cipher dma set mask failed.\n");
        return ret;
    }

    mem->dma_size = size;
    mem->dma_virt = dma_alloc_coherent(dev, mem->dma_size, (dma_addr_t *)(&ADDR_U64(mem->dma_addr)), GFP_ATOMIC);
    if(HI_NULL == mem->dma_virt)
    {
        HI_LOG_ERROR("dma_alloc_coherent error.\n");
        return HI_FAILURE;
    }
    ADDR_U64(mem->mmz_addr) = ADDR_U64(mem->dma_addr);

    /* save the map info */
    for(i=0; i<CRYPTO_MEM_MAP_TABLE_DEPTH; i++)
    {
        if (HI_FALSE == loacl_map_table[i].valid)
        {
            ADDR_U64(loacl_map_table[i].dma) = ADDR_U64(mem->dma_addr);
            loacl_map_table[i].via = mem->dma_virt;
            loacl_map_table[i].valid= HI_TRUE;
            HI_LOG_DEBUG("map local map %d, dam 0x%x, via 0x%p\n",
                i, ADDR_U64(mem->dma_addr), mem->dma_virt);
            break;
        }
    }

    return HI_SUCCESS;
}

static s32 cipher_dma_free_coherent(crypto_mem *mem)
{
     struct device *dev = HI_NULL;
     s32 ret = HI_FAILURE;
     s32 i = 0;
     crypto_mem mem_temp;

     if (HI_NULL == mem)
     {
        HI_LOG_ERROR("mem is null.\n");
        return HI_FAILURE;
     }

     crypto_memset(&mem_temp, sizeof(mem_temp), 0, sizeof(mem_temp));
     crypto_memcpy(&mem_temp, sizeof(mem_temp), mem, sizeof(crypto_mem));

     dev = (struct device *)cipher_get_device();
     if (NULL == dev)
     {
         HI_LOG_ERROR("cipher_get_device error.\n");
         return HI_FAILURE;
     }

     ret = cipher_dma_set_mask(dev);
     if (HI_SUCCESS != ret)
     {
         HI_LOG_ERROR("cipher dma set mask failed.\n");
         return ret;
     }

     dma_free_coherent(dev, mem->dma_size, mem->dma_virt, ADDR_U64(mem->dma_addr));

     /* remove the map info */
     for(i=0; i<CRYPTO_MEM_MAP_TABLE_DEPTH; i++)
     {
         if ( loacl_map_table[i].valid &&
             ADDR_U64(loacl_map_table[i].dma) == ADDR_U64(mem_temp.dma_addr))
         {
             ADDR_U64(loacl_map_table[i].dma) = 0x00;
             loacl_map_table[i].via = HI_NULL;
             loacl_map_table[i].valid = HI_FALSE;
             HI_LOG_DEBUG("unmap local map %d, dam 0x%x, via 0x%p\n",
                 i, ADDR_U64(mem_temp.dma_addr), mem_temp.dma_virt);
             break;
         }
     }
     crypto_memset(mem, sizeof(crypto_mem), 0, sizeof(crypto_mem));

     return HI_SUCCESS;
}

/*brief allocate and map a mmz or smmu memory
* we can't allocate smmu directly during TEE boot period.
* in addition, the buffer of cipher node list must be mmz.
* so here we have to allocate a mmz memory then map to smmu if necessary.
*/
static s32 hash_mem_alloc_remap(crypto_mem *mem, u32 type, char const *name, u32 size)
{
    u32 i;
    crypto_memset(mem, sizeof(crypto_mem), 0, sizeof(crypto_mem));

    HI_LOG_DEBUG("mem_alloc_remap()- name %s, size 0x%x\n", name, size);

    mem->dma_size = size;
    mem->dma_virt = kzalloc(size, GFP_KERNEL);
    if(NULL == mem->dma_virt)
    {
        return HI_ERR_CIPHER_FAILED_MEM;
    }

    ADDR_U64(mem->mmz_addr) = virt_to_phys(mem->dma_virt);
    if(0 == ADDR_U64(mem->mmz_addr))
    {
        if(HI_NULL != mem->dma_virt)
        {
             kfree(mem->dma_virt);
             mem->dma_virt = HI_NULL;
        }

        return HI_ERR_CIPHER_FAILED_MEM;
    }

    ADDR_U64(mem->dma_addr) = ADDR_U64(mem->mmz_addr);

    HI_LOG_DEBUG("MMZ/MMU malloc, MMZ 0x%x, MMZ/MMU 0x%x, VIA 0x%p, SIZE 0x%x\n",
        ADDR_U64(mem->mmz_addr), ADDR_U64(mem->dma_addr), mem->dma_virt, size);

    mem->user_buf = HI_NULL;

    /* save the map info */
    for(i=0; i<CRYPTO_MEM_MAP_TABLE_DEPTH; i++)
    {
        if (HI_FALSE == loacl_map_table[i].valid)
        {
            ADDR_U64(loacl_map_table[i].dma) = ADDR_U64(mem->dma_addr);
            loacl_map_table[i].via = mem->dma_virt;
            loacl_map_table[i].valid= HI_TRUE;
            HI_LOG_DEBUG("map local map %d, dam 0x%x, via 0x%p\n",
                i, ADDR_U64(mem->dma_addr), mem->dma_virt);
            break;
        }
    }

    return HI_SUCCESS;
}

/*brief release and unmap a mmz or smmu memory */
static s32 hash_mem_release_unmap(crypto_mem *mem)
{
    u32 i;

    kfree(mem->dma_virt);

    /* remove the map info */
    for(i=0; i<CRYPTO_MEM_MAP_TABLE_DEPTH; i++)
    {
        if ( loacl_map_table[i].valid &&
            ADDR_U64(loacl_map_table[i].dma) == ADDR_U64(mem->dma_addr))
        {
            ADDR_U64(loacl_map_table[i].dma) = 0x00;
            loacl_map_table[i].via = HI_NULL;
            loacl_map_table[i].valid = HI_FALSE;
            HI_LOG_DEBUG("unmap local map %d, dam 0x%x, via 0x%p\n",
                i, ADDR_U64(mem->dma_addr), mem->dma_virt);
            break;
        }
    }
    crypto_memset(mem, sizeof(crypto_mem), 0, sizeof(crypto_mem));

    return HI_SUCCESS;
}

static s32 crypto_mem_alloc_remap(crypto_mem *mem, u32 type, char const *name, u32 size)
{
    return cipher_dma_alloc_coherent(mem, type, name, size);
}

/*brief release and unmap a mmz or smmu memory */
static s32 crypto_mem_release_unmap(crypto_mem *mem)
{
    return cipher_dma_free_coherent(mem);
}

/*brief map a mmz or smmu memory */
static s32 crypto_mem_map(crypto_mem *mem)
{
    u32 i;

    HI_LOG_DEBUG("crypto_mem_map()- dma 0x%x, size 0x%x\n",
        ADDR_U64(mem->dma_addr), mem->dma_size);

    /* try to query the table to get cpu address firstly,
     * if can't get cpu address from the table, then call system api to map it.
     */
    for(i=0; i<CRYPTO_MEM_MAP_TABLE_DEPTH; i++)
    {
        if ( loacl_map_table[i].valid &&
            ADDR_U64(loacl_map_table[i].dma) == ADDR_U64(mem->dma_addr))
        {
            mem->dma_virt = loacl_map_table[i].via;
            HI_LOG_DEBUG("local map %d, dam 0x%x, via 0x%p\n",
                i, ADDR_U64(mem->dma_addr), mem->dma_virt);
            return HI_SUCCESS;
        }
    }

    mem->dma_virt = (HI_U8*)phys_to_virt(ADDR_U64(mem->dma_addr));
    if(HI_NULL == mem->dma_virt)
    {
        return HI_ERR_CIPHER_FAILED_MEM;
    }

    HI_LOG_INFO("crypto_mem_map()- via 0x%p\n", mem->dma_virt);

    return HI_SUCCESS;

}

/*brief unmap a mmz or smmu memory */
static s32 crypto_mem_unmap(crypto_mem *mem)
{
    u32 i;

    HI_LOG_DEBUG("crypto_mem_unmap()- dma 0x%x, size 0x%x\n",
        ADDR_U64(mem->dma_addr), mem->dma_size);

    /* try to query the table to ummap cpu address firstly,
     * if can't get cpu address from the table, then call system api to unmap it.
     */
    for(i=0; i<CRYPTO_MEM_MAP_TABLE_DEPTH; i++)
    {
        if ( loacl_map_table[i].valid &&
            ADDR_U64(loacl_map_table[i].dma) == ADDR_U64(mem->dma_addr))
        {
            /* this api can't unmap the dma within the map table */
            HI_LOG_DEBUG("local unmap %d, dam 0x%x, via 0x%p\n",
                i, ADDR_U64(mem->dma_addr), mem->dma_virt);
            return HI_SUCCESS;
        }
    }

    return HI_SUCCESS;
}

void crypto_cpuc_flush_dcache_area(void *kvir, u32 length)
{
    crypto_flush_dcache_area(kvir, length);
}

void crypto_mem_init(void)
{
    crypto_memset(&loacl_map_table, sizeof(loacl_map_table), 0, sizeof(loacl_map_table));
}

void crypto_mem_deinit(void)
{

}

s32 crypto_mem_create(crypto_mem *mem, u32 type, const char *name, u32 size)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    return crypto_mem_alloc_remap(mem, type, name, size);
}

s32 crypto_mem_destory(crypto_mem *mem)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    return crypto_mem_release_unmap(mem);
}

s32 hash_mem_create(crypto_mem *mem, u32 type, const char *name, u32 size)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    return hash_mem_alloc_remap(mem, type, name, size);
}

s32 hash_mem_destory(crypto_mem *mem)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    return hash_mem_release_unmap(mem);
}

s32 crypto_mem_open(crypto_mem *mem, compat_addr dma_addr, u32 dma_size)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    mem->dma_addr = dma_addr;
    mem->dma_size = dma_size;

    return crypto_mem_map(mem);
}

s32 crypto_mem_close(crypto_mem *mem)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    return crypto_mem_unmap(mem);;
}

s32 crypto_mem_attach(crypto_mem *mem, void *buffer)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    mem->user_buf = buffer;

    return HI_SUCCESS;
}

s32 crypto_mem_flush(crypto_mem *mem, u32 dma2user, u32 offset, u32 data_size)
{
    CRYPTO_ASSERT(HI_NULL != mem);
    CRYPTO_ASSERT(HI_NULL != mem->dma_virt);
    CRYPTO_ASSERT(HI_NULL != mem->user_buf);
    CRYPTO_ASSERT(data_size <= mem->dma_size);

    if (dma2user)
    {
        crypto_memcpy((u8*)mem->user_buf + offset, data_size,
            (u8*)mem->dma_virt + offset, data_size);
    }
    else
    {
        crypto_memcpy((u8*)mem->dma_virt + offset, data_size,
            (u8*)mem->user_buf + offset, data_size);
    }

    return HI_SUCCESS;
}

s32 crypto_mem_phys(crypto_mem *mem, compat_addr *dma_addr)
{
    CRYPTO_ASSERT(HI_NULL != mem);

    dma_addr->phy = ADDR_U64(mem->dma_addr);

    return HI_SUCCESS;
}

void * crypto_mem_virt(crypto_mem *mem)
{
    if (HI_NULL == mem)
    {
        return HI_NULL;
    }

    return mem->dma_virt;
}

s32 crypto_copy_from_user(void *to, const void  *from, unsigned long n)
{
    if (0 == n)
    {
        return HI_SUCCESS;
    }

    HI_LOG_CHECK_PARAM(HI_NULL == to);
    HI_LOG_CHECK_PARAM(HI_NULL == from);

    return copy_from_user(to, from, n);
}

s32 crypto_copy_to_user(void *to, const void  *from, unsigned long n)
{
    if (0 == n)
    {
        return HI_SUCCESS;
    }

    HI_LOG_CHECK_PARAM(HI_NULL == to);
    HI_LOG_CHECK_PARAM(HI_NULL == from);

    return copy_to_user(to, from, n);
}

u32 crypto_is_sec_cpu(void)
{
    return module_get_secure();
}

void smmu_get_table_addr(u64 *rdaddr, u64 *wraddr, u64 *table)
{
#ifdef CRYPTO_SMMU_SUPPORT
    u32 smmu_e_raddr, smmu_e_waddr, mmu_pgtbl;
    HI_DRV_SMMU_GetPageTableAddr(&mmu_pgtbl, &smmu_e_raddr, &smmu_e_waddr);

    *rdaddr = smmu_e_raddr;
    *wraddr = smmu_e_waddr;
    *table = mmu_pgtbl;
#else
    *rdaddr = 0x00;
    *wraddr = 0x00;
    *table  = 0x00;
#endif
}

s32 crypto_waitdone_callback(void *param)
{
    u32 *pbDone = param;

    return  *pbDone != HI_FALSE;
}

s32 cipher_check_mmz_phy_addr(u64 u64PhyAddr, u64 u64Len)
{
#ifdef CIPHER_CHECK_MMZ_PHY
    hil_mmb_t *mmb = HI_NULL;
    unsigned long mmb_offset = 0;

    /* Check wether the start address is within the MMZ range of the current system. */
    mmb = hil_mmb_getby_phys_2(u64PhyAddr, &mmb_offset);
    if(NULL != mmb)
    {
        /* Check wether the end address is within the MMZ range of the current system */
        mmb = hil_mmb_getby_phys_2(u64PhyAddr + u64Len -1, &mmb_offset);
        if(NULL == mmb)
        {
            HI_LOG_PrintFuncErr(hil_mmb_getby_phys_2, HI_FAILURE);
            return HI_FAILURE;
        }
    }
    else/* Whether the starting address is within the MMZ range of other systems */
    {
        if (hil_map_mmz_check_phys(u64PhyAddr, u64Len))
        {
            HI_LOG_PrintFuncErr(hil_map_mmz_check_phys, HI_FAILURE);
            return HI_FAILURE;
        }
    }
#endif

#ifdef CIPHER_BUILDIN
    /*check physical addr is ram region*/
    if (pfn_valid(u64PhyAddr >> PAGE_SHIFT) || pfn_valid(u64Len + (u64PhyAddr >> PAGE_SHIFT)))
    {
#if defined(CONFIG_CMA) && defined(CONFIG_ARCH_HISI_BVT)
        if(is_hicma_address(u64PhyAddr, u64Len))
        {
            return HI_SUCCESS;
        }
        else
        {
            HI_LOG_PrintFuncErr(is_hicma_address, HI_FAILURE);
            return HI_FAILURE;
        }
#endif
        HI_LOG_ERROR("physical addr is ram region.\n");
        return HI_FAILURE;
    }
    else
    {
        return HI_SUCCESS;
    }
#endif

    return HI_SUCCESS;
}

/** @}*/  /** <!-- ==== API Code end ====*/
