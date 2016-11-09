/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <current.h>
#include <synch.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <thread.h>
#include <mips/tlb.h>
#include <vm.h>
#include <uio.h>
#include <vfs.h>
#include <spl.h>
#include <vnode.h>

#define STACKPAGES 12
/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

/*
 * OPT_DUMBVM is already unset. Demand paging is implement. Do read the given document first!!
 * Swap file is used for it. Pure paging is used. No segamenting is used.
 */
void stats_init(void)
{
	VM_STATS->tlb_misses = 0;
	VM_STATS->vm_fault_with_free_page = 0;
	VM_STATS->vm_fault_with_lru = 0;
	VM_STATS->page_fault = 0;
	VM_STATS->tlb_misses_with_page_in_memory = 0;
}

void print_stats(void)
{
	lock_acquire(vm_metrics_lock);
	kprintf("Number of TLB misses are : %d\n",VM_STATS->tlb_misses);
	kprintf("Number of TLB misses where page was found in memory : %d\n",VM_STATS->tlb_misses_with_page_in_memory);
	kprintf("Number of page faults : %d\n",VM_STATS->page_fault);
	kprintf("Number of page faults where free page was found : %d\n",VM_STATS->vm_fault_with_free_page);
	kprintf("Number of page faults where LRU was used : %d\n",VM_STATS->vm_fault_with_lru);
	lock_release(vm_metrics_lock);
}


void
vm_bootstrap(void)
{
	
	uint32_t firstpaddr = 0; // address of first free physical page 
	uint32_t lastpaddr = 0; // one past end of last free physical page
	
	vm_lock = lock_create("vm_lock");

	vm_metrics_lock = lock_create("vm_metrics_lock");
	
	lock_acquire(vm_lock);
	VM = kmalloc(sizeof(struct vm_manager));
	//kprintf("VM address is %x\n",VM);
	VM->page_frame_table = kmalloc(sizeof(struct vm_manager_page_entry)*VM_PAGES);
	//kprintf("VM address is %x\n",VM->page_frame_table);

	VM_STATS = kmalloc(sizeof(struct vm_metrics));
	stats_init();

	firstpaddr = ram_stealmem(VM_PAGES);
	lastpaddr = ram_stealmem(0);

	uint32_t coremap_size = (lastpaddr - firstpaddr)/PAGE_SIZE;
	kprintf("Virtual Memory: %d pages in memory\n", coremap_size);
	kprintf("Virtual Memory: Page frames initialised : START : %0x END : %0x\n",PADDR_TO_KVADDR(firstpaddr),PADDR_TO_KVADDR(lastpaddr));

	int num_pages = coremap_size;
	int i;
	VM->num_page_frames = num_pages;
	for (i = 0 ; i < num_pages ; i++)
	{
		(VM->page_frame_table[i]).p_address = firstpaddr + i*PAGE_SIZE;
		(VM->page_frame_table[i]).v_address = 0;
		(VM->page_frame_table[i]).thread_id = -1;
		(VM->page_frame_table[i]).is_free = true;
		(VM->page_frame_table[i]).reference_bit = false;
		(VM->page_frame_table[i]).dirty_bit = false;
		(VM->page_frame_table[i]).valid_bit = false;
	}

	lock_release(vm_lock);

	vm_ready = 1;
	
}

static
paddr_t
getppages(unsigned long npages)
{
        paddr_t addr;
        spinlock_acquire(&stealmem_lock);
        
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
        return addr;
}


static
paddr_t
getpage(unsigned long npages)
{
	/*implement it using vm manager*/
	
	struct vm_manager_page_entry* page_entry;
	int i;
	int totpages;
	struct addrspace *as;
	if (vm_ready)
	{
		//kprintf("Virtual Memory: getpage %ld\n", npages);
		lock_acquire(vm_lock);
		if (npages > 1)
		{
			panic("Virtual Memory: More than 1 pages required, cannot be allocated");
		}
		else
		{
			totpages = VM->num_page_frames;
			for (i = 0 ; i < totpages ; i++)
			{
				page_entry = &(VM->page_frame_table[i]);
				if (page_entry->is_free)
				{
					VM_STATS->vm_fault_with_free_page++;
					lock_release(vm_lock);
					return page_entry->p_address;
				}
				/* Free page found, physical address returned */
			}
			//kprintf("using lru\n");
			VM_STATS->vm_fault_with_lru++;
			/* No free page found, use page replacement (LRU) */
			int id_thread;
			for (i = 0 ; i < totpages ; i++)
			{
				page_entry = &(VM->page_frame_table[i]);
				id_thread = page_entry->thread_id;
				if (!page_entry->reference_bit)
				{
					if (page_entry->dirty_bit)
					{
						/* Write page to swap file of the process whose page is being replaced */
						as = page_entry->as;
						if(as==NULL) panic("process not found in run queue\n");
						int result = write_page_to_swap(page_entry->p_address,page_entry->v_address,as);
						if(result)
							return result;
						
					}

					update_pagetable(page_entry);
					
					/* Make changes to page table of process whose page is being replaced */
					
					lock_release(vm_lock);
					return page_entry->p_address;
				}
			}

			/* All pages have reference bit as true. Returning a random page */

			page_entry = VM->page_frame_table;
			id_thread = page_entry->thread_id;
			if (page_entry->dirty_bit)
			{
				/* Write first page of physical memory of the process whose page is being replaced, to swap */
				as = page_entry->as;;
				if(as==NULL) panic("process not found in run queue\n");
				int result = write_page_to_swap(page_entry->p_address,page_entry->v_address,as);
				if(result)
					return result;
			}
			
			update_pagetable(page_entry);
			/* Make changes to page table of process whose page is being replaced */


			lock_release(vm_lock);
			return page_entry->p_address;
		}
		
	}
	else
	{
		return ram_stealmem(npages);
	}
	return 0;
}


struct addrspace *
as_create(void)
{
	//kprintf("Virtual Memory: as_create\n");
	struct addrspace *as;
	int i;
	as = kmalloc(sizeof(struct addrspace));
	//kprintf("%d\n",sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initializing page table for the address space.
	 */
	as->as_stackpbase=0;
	as->as_pagetable.pt_totalpages=0;
	for(i=0;i<PAGE_TABLE_ENTRIES;i++)
	{
		as->as_pagetable.pt_entrytable[i].vbit=false;
		as->as_pagetable.pt_entrytable[i].rbit=false;
		as->as_pagetable.pt_entrytable[i].dbit=false;
		as->as_pagetable.pt_entrytable[i].vaddress=0;
		as->as_pagetable.pt_entrytable[i].paddress=0;
	}
	/*
	 * Initializing swap file for the address space.
	 */
	as->as_sf.sf_file[0]='s';
	as->as_sf.sf_file[1]='f';
	as->as_sf.sf_file[2]= (char)((vaddr_t)as & 0x0000000f);
	as->as_sf.sf_file[3]= (char)((vaddr_t)as & 0x000000f0);
	as->as_sf.sf_file[4]= (char)((vaddr_t)as & 0x00000f00);
	as->as_sf.sf_file[5]= (char)((vaddr_t)as & 0x0000f000);
	as->as_sf.sf_file[6]= (char)((vaddr_t)as & 0x000f0000);
	as->as_sf.sf_file[7]= (char)((vaddr_t)as & 0x00f00000);
	as->as_sf.sf_file[8]= (char)((vaddr_t)as & 0x0f000000);
	as->as_sf.sf_file[9]= (char)((vaddr_t)as & 0xf0000000);
	as->as_sf.sf_pages = 0;

	return as;
}

/*
 * This function is not tested but implemented just for the sake of completion.
 */

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	//kprintf("Virtual Memory: as_copy\n");
	struct addrspace *newas;
	int i;
	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	newas->as_pagetable.pt_totalpages=old->as_pagetable.pt_totalpages;
	for(i=0;i<newas->as_pagetable.pt_totalpages;i++)
	{
		newas->as_pagetable.pt_entrytable[i].vaddress=old->as_pagetable.pt_entrytable[i].vaddress;
		if(old->as_pagetable.pt_entrytable[i].vbit)
		{
			newas->as_pagetable.pt_entrytable[i].paddress = getpage(1);
			if (newas->as_pagetable.pt_entrytable[i].paddress == 0) {
				return ENOMEM;
			}

			KASSERT(newas->as_pagetable.pt_entrytable[i].paddress != 0);
			
			newas->as_pagetable.pt_entrytable[i].vbit=true;
			memmove((void *)PADDR_TO_KVADDR(newas->as_pagetable.pt_entrytable[i].paddress),
			(const void *)PADDR_TO_KVADDR(old->as_pagetable.pt_entrytable[i].paddress),
			PAGE_SIZE);
		}
	}
	
	newas->as_sf.sf_file[0]='s';
	newas->as_sf.sf_file[1]='f';
	newas->as_sf.sf_file[2]= (char)((vaddr_t)newas & 0x0000000f);
	newas->as_sf.sf_file[3]= (char)((vaddr_t)newas & 0x000000f0);
	newas->as_sf.sf_file[4]= (char)((vaddr_t)newas & 0x00000f00);
	newas->as_sf.sf_file[5]= (char)((vaddr_t)newas & 0x0000f000);
	newas->as_sf.sf_file[6]= (char)((vaddr_t)newas & 0x000f0000);
	newas->as_sf.sf_file[7]= (char)((vaddr_t)newas & 0x00f00000);
	newas->as_sf.sf_file[8]= (char)((vaddr_t)newas & 0x0f000000);
	newas->as_sf.sf_file[9]= (char)((vaddr_t)newas & 0xf0000000);
	newas->as_sf.sf_pages = old->as_sf.sf_pages ; 
	for(i=0;i<newas->as_sf.sf_pages;i++)
	{
		newas->as_sf.sf_pagemapping[i].vaddress = old->as_sf.sf_pagemapping[i].vaddress;
		newas->as_sf.sf_pagemapping[i].size = old->as_sf.sf_pagemapping[i].size;
		newas->as_sf.sf_pagemapping[i].is_stack = old->as_sf.sf_pagemapping[i].is_stack;
	}
	
	/*
	*
	*Copy swap file 
	*
	*/
	struct vnode* vnode_ret;
	int result = vfs_open(newas->as_sf.sf_file, 6, 0664, &vnode_ret);
    if(result)
    {
        kprintf("error while opening file\n");
        return result;
    }
    
    vfs_close(vnode_ret);
    
	copy_swap_file(&(old->as_sf),&(newas->as_sf));
		
	*ret = newas;
	return 0;

}

void
as_destroy(struct addrspace *as)
{
	//kprintf("Virtual Memory: as_destroy\n");
	print_stats();
	kfree(as);
}

void
as_activate(struct addrspace *as)
{
	//kprintf("Virtual Memory: as_activate\n");
	int i, spl;

	(void)as;

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);

}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	//kprintf("Virtual Memory: as_define_region on vaddr: %x with size %d ",vaddr,sz);
	int npages; 
	int i,j;
	vaddr_t vd = vaddr;
	size_t memsize = sz;
	/* Align the region. First, the base... */
	sz += vd & ~(vaddr_t)PAGE_FRAME;
	vd &= PAGE_FRAME;
	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;
	//kprintf("and with pages %d\n",npages);
	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	
	KASSERT(as->as_pagetable.pt_totalpages+npages<PAGE_TABLE_ENTRIES);
	KASSERT(as->as_pagetable.pt_totalpages == as->as_sf.sf_pages);
	
	/* 
	 * set up the index for page table entry. Page table is index by virtual address
	 * of the user address space.
	 */
		
	for(i=as->as_pagetable.pt_totalpages;i<npages+as->as_pagetable.pt_totalpages;i++)
	{
		as->as_pagetable.pt_entrytable[i].vaddress=vd;
		vd+=PAGE_SIZE;
	}
	as->as_pagetable.pt_totalpages+=npages;
	
	vd = vaddr & PAGE_FRAME;
	
	/*
	 * set up swap file index. Each array index is the a offset(offset * PAGE_SIZE) in the 
	 * swap file of the page starting at virtual address stored at that index.
	 */
	   
	for(i=as->as_sf.sf_pages,j=1;i<as->as_sf.sf_pages+npages;i++,j++)
	{
		if(j==1)
		{
			as->as_sf.sf_pagemapping[i].vaddress = vaddr; 
			if(memsize+vaddr-vd <= PAGE_SIZE)
				as->as_sf.sf_pagemapping[i].size = memsize;	
			else
				as->as_sf.sf_pagemapping[i].size = PAGE_SIZE+vd-vaddr;
			
		}
		else if(j==npages)
		{
			as->as_sf.sf_pagemapping[i].vaddress = vd;
			as->as_sf.sf_pagemapping[i].size = memsize;	
		}
		else
		{
			as->as_sf.sf_pagemapping[i].vaddress = vd;
			as->as_sf.sf_pagemapping[i].size = PAGE_SIZE;
		}
		vd += PAGE_SIZE;
		memsize-=as->as_sf.sf_pagemapping[i].size;
		as->as_sf.sf_pagemapping[i].is_stack = false;
		
	}
	as->as_sf.sf_pages+=npages;
	(void)executable;
	return 0;
}
/* 
 * it does nothing as no page is loaded in memory due to demand paging. 
 * Just some set assertions.
 */

int
as_prepare_load(struct addrspace *as)
{
	//kprintf("Virtual Memory: as_prepare_load\n");
	int i;
	
	for(i=0;i<as->as_pagetable.pt_totalpages;i++)
	{
		KASSERT(as->as_pagetable.pt_entrytable[i].paddress==0);
	}
	KASSERT(as->as_stackpbase == 0);
	
	/*kprintf("printing addr space\n");
	for(i=0;i<as->as_pagetable.pt_totalpages;i++)
	{
		kprintf("%x\n",as->as_pagetable.pt_entrytable[i].vaddress);
	}
	
	kprintf("printing swap space\n");
	for(i=0;i<as->as_sf.sf_pages;i++)
	{
		kprintf("%x %d\n",as->as_sf.sf_pagemapping[i].vaddress,as->as_sf.sf_pagemapping[i].size);
	}*/
	
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

/*
 * stack grows from higher address to lower
 */
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	
	
	int i,j;
	
	for (i = as->as_pagetable.pt_totalpages,j=1; i < STACKPAGES + as->as_pagetable.pt_totalpages;j++, i++)
	{
		as->as_pagetable.pt_entrytable[i].vaddress = USERSTACK - j * PAGE_SIZE;
	}
	as->as_pagetable.pt_totalpages+=STACKPAGES;
	
	for(i=as->as_sf.sf_pages,j=1;i<as->as_sf.sf_pages+STACKPAGES;i++,j++)
	{
		as->as_sf.sf_pagemapping[i].vaddress = USERSTACK - j * PAGE_SIZE;
		as->as_sf.sf_pagemapping[i].size = PAGE_SIZE;
		as->as_sf.sf_pagemapping[i].is_stack = true;
	}
	as->as_sf.sf_pages+=STACKPAGES;
	
	/*
	 * filling up the stack pages of swap file with zeros
	 */
	
	void * ktemp = kmalloc(PAGE_SIZE);
	struct iovec sfiov;
	struct uio sfu;
	struct vnode * sfv;
	off_t sfoffset = as->as_sf.sf_pages - STACKPAGES;
	int result;
	result = vfs_open(as->as_sf.sf_file, 2, 0664, &sfv);
	if (result) {
		return result;
	}
	for(i =0;i<STACKPAGES;i++)
	{
		bzero(ktemp, PAGE_SIZE);
		sfiov.iov_ubase = ktemp;
		sfiov.iov_len = PAGE_SIZE;
		sfu.uio_iov = &sfiov;
		sfu.uio_iovcnt = 1;
		sfu.uio_resid = PAGE_SIZE;
		sfu.uio_offset = sfoffset*PAGE_SIZE;
		sfu.uio_segflg = UIO_SYSSPACE;
		sfu.uio_rw = UIO_WRITE;
		sfu.uio_space = NULL;
	}
	kfree(ktemp);
	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;
	//kprintf("Virtual Memory: as_define_stack : stack : %x\n",*stackptr);
	
	return 0;
}

/*
 * brings in the page from swap file to main memory pointed by paddr
 */
 
int
read_page_from_swap(paddr_t paddr, vaddr_t vaddr, struct addrspace *as)
{
	//kprintf("Virtual Memory: read_page_from_swap : vaddr : %x to paddr : %x\n",vaddr,paddr);
	struct vnode *v;
	struct iovec iov;
	struct uio u;
	int i,result;
	off_t offset=-1;
	vaddr_t load_vaddr;
	size_t load_size;
	bool load_is_stack;
	struct swap_file* sf = &(as->as_sf);
	/* Open the file. */
	result = vfs_open(sf->sf_file, 0, 0664, &v);
	if (result) {
		return result;
	}
	
	for(i=0;i<sf->sf_pages;i++)
	{
		if((sf->sf_pagemapping[i].vaddress & PAGE_FRAME) == (vaddr & PAGE_FRAME))
		{
			 offset = i;
			 load_vaddr =  sf->sf_pagemapping[i].vaddress;
			 load_size = sf->sf_pagemapping[i].size;
			 load_is_stack = sf->sf_pagemapping[i].is_stack;
			 break;
		}
	}
	KASSERT(offset!=-1);
	
	iov.iov_ubase = (void*)PADDR_TO_KVADDR(paddr);
	iov.iov_len = load_size;
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = load_size;
	u.uio_offset = offset*PAGE_SIZE;
	u.uio_segflg = UIO_SYSSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = NULL;

	result = VOP_READ(v, &u);
	if (result) {
		return result;
	}
	vfs_close(v);
	/*
	if((vaddr & PAGE_FRAME) == 0x400000){
	int k;
	kprintf("printing segment\n");
	for(k=0;k<sf->sf_pagemapping[i].size;k++)
		kprintf("%d ",(int)*((char*)(PADDR_TO_KVADDR(paddr)+k)));
	kprintf("\nend printing segment\n");
	}
	*/
	return 0;
}

/*
 * writes back the page from swap file to main memory pointed by paddr
 */

int
write_page_to_swap(paddr_t paddr, vaddr_t vaddr, struct addrspace *as)
{
	//kprintf("Virtual Memory: write_page_to_swap : vaddr : %x to paddr : %x\n",vaddr,paddr);
	struct vnode *v;
	struct iovec iov;
	struct uio u;
	int i,result;
	off_t offset;
	vaddr_t load_vaddr;
	size_t load_size;
	bool load_is_stack;
	struct swap_file* sf = &(as->as_sf);
	/*int k;
	kprintf("printing segment\n");
	for(k=0;k<sf->sf_pagemapping[i].size;k++)
		kprintf("%d ",(int)*((char*)(PADDR_TO_KVADDR(paddr)+k)));
	kprintf("\nend printing segment\n");
	*/
	/* Open the file. */
	result = vfs_open(sf->sf_file, 1, 0664, &v);
	if (result) {
		return result;
	}
	
	for(i=0;i<sf->sf_pages;i++)
	{
		if((sf->sf_pagemapping[i].vaddress & PAGE_FRAME) == (vaddr & PAGE_FRAME))
		{
			 offset = i;
			 load_vaddr =  sf->sf_pagemapping[i].vaddress;
			 load_size = sf->sf_pagemapping[i].size;
			 load_is_stack = sf->sf_pagemapping[i].is_stack;
			 break;
		}
	}
	KASSERT(offset!=-1);
	iov.iov_ubase = (void*)PADDR_TO_KVADDR(paddr);
	iov.iov_len = load_size;
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = load_size;
	u.uio_offset = offset*PAGE_SIZE;
	u.uio_segflg = UIO_SYSSPACE;
	u.uio_rw = UIO_WRITE;
	u.uio_space = NULL;

	result = VOP_WRITE(v, &u);
	if (result) {
		return result;
	}
	vfs_close(v);
	
	return 0;
}

/* 
 * not tested
 */
int copy_swap_file(struct swap_file* from, struct swap_file* to)
{
	struct vnode *vfrom, *vto;
	struct iovec iovfrom, iovto;
	struct uio ufrom,uto;
	int i,result;
	
	/* Open the file. */
	result = vfs_open(from->sf_file, 0, 0664, &vfrom);
	if (result) {
		return result;
	}
	result = vfs_open(to->sf_file, 1, 0664, &vto);
	if (result) {
		return result;
	}
	void *buf = kmalloc(PAGE_SIZE);
	for(i=0;i<from->sf_pages;i++)
	{
		uio_kinit(&iovfrom, &ufrom, (void*)buf, from->sf_pagemapping[i].size, i*PAGE_SIZE, UIO_READ);
		result = VOP_READ(vfrom, &ufrom);
		if (result) {
			return result;
		}
		
		uio_kinit(&iovto, &uto, (void*)buf, from->sf_pagemapping[i].size, i*PAGE_SIZE, UIO_WRITE);
		result = VOP_WRITE(vto, &uto);
		if (result) {
			return result;
		}
	}
	
	vfs_close(vfrom);
	vfs_close(vto);
	
	return 0;	
}
#if 0
int
load_segment_to_swap_file(void* vaddr1,vaddr_t r_vaddr, size_t memsize)
{
	/*int k;
	kprintf("printing segment\n");
	for(k=0;k<memsize;k++)
		kprintf("%c ",*((char*)(vaddr1+k)));
	kprintf("\nend printing segment\n");
	*/
	size_t sz = memsize,offset,len; 
	int i,j,npages;
	vaddr_t vaddr = (vaddr_t)vaddr1,loaded_vaddr; 
	kprintf("load_elf: load_segment_to_swap_file : buffer vaddr : %x actual vaddr : %x\n",vaddr,r_vaddr);
	vaddr_t vd = vaddr;
	struct addrspace *as = curthread->t_addrspace;
	struct swap_file *sf = &(as->as_sf);
	struct iovec iov;
	struct uio u;
	struct vnode * v;
	int result;

	sz += vd & ~(vaddr_t)PAGE_FRAME;
	vd &= PAGE_FRAME;
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;
	
	r_vaddr &= r_vaddr;
	
	result = vfs_open(sf->sf_file, 6, 0664, &v);
	if (result) {
		return result;
	}
	for(i=1;i<=npages;i++)
	{
		for(j=0;j<sf->sf_pages;j++)
		{
			if((sf->sf_pagemapping[j].vaddress & PAGE_FRAME) == (r_vaddr & PAGE_FRAME))
			{
				 offset = j;
				 break;
			}
		}
		
		if(i==1)
		{
			iov.iov_ubase = (void*)vaddr;
			if(memsize+vaddr-vd <= PAGE_SIZE)
				iov.iov_len = memsize;	
			else
				iov.iov_len = PAGE_SIZE+vd-vaddr;
			u.uio_iov = &iov;
			u.uio_iovcnt = 1;
			u.uio_resid = iov.iov_len;          // amount to read from the file
			u.uio_offset = offset*PAGE_SIZE;
			u.uio_segflg = UIO_SYSSPACE;
			u.uio_rw = UIO_WRITE;
			u.uio_space = NULL;
			len = iov.iov_len;
			loaded_vaddr = iov.iov_ubase; 
		}
		else if(i==npages)
		{
			iov.iov_ubase = (void*)vd;
			iov.iov_len = memsize;
			u.uio_iov = &iov;
			u.uio_iovcnt = 1;
			u.uio_resid = memsize;          // amount to read from the file
			u.uio_offset = offset*PAGE_SIZE;
			u.uio_segflg = UIO_SYSSPACE;
			u.uio_rw = UIO_WRITE;
			u.uio_space = NULL;
			len = iov.iov_len;	
			loaded_vaddr = iov.iov_ubase;
		}
		else
		{
			iov.iov_ubase = (void*)vd;
			iov.iov_len = PAGE_SIZE;
			u.uio_iov = &iov;
			u.uio_iovcnt = 1;
			u.uio_resid = PAGE_SIZE;          // amount to read from the file
			u.uio_offset = offset*PAGE_SIZE;
			u.uio_segflg = UIO_SYSSPACE;
			u.uio_rw = UIO_WRITE;
			u.uio_space = NULL;
			len = iov.iov_len;
			loaded_vaddr = iov.iov_ubase;
		}
		
		
		
		result = VOP_WRITE(v, &u);
		if (result) {
			return result;
		}
		kprintf("load_elf: loaded segment to swap file : offset : %d size : %d buffer vaddr : %x actual vaddr : %x\n",offset,len,loaded_vaddr,r_vaddr);
		vd += PAGE_SIZE;
		r_vaddr += PAGE_SIZE;
		memsize-=len;
	}
	return 0;
}
#endif

int update_page_frame_entry(vaddr_t vaddr_fault, paddr_t paddr_fault, bool vbit, bool dbit, int id_thread,struct addrspace* as)
{
	//kprintf("Virtual Memory: update_page_frame_entry : vaddr : %x paddr: %x\n",vaddr_fault,paddr_fault);
	lock_acquire(vm_lock);
	struct vm_manager_page_entry* page_frame_entry;
	int totpages = VM->num_page_frames;
	int i;

	for (i = 0 ; i < totpages ; i++)
	{
		page_frame_entry = &(VM->page_frame_table[i]);
		if (page_frame_entry->p_address == paddr_fault)
		{
			page_frame_entry->thread_id = id_thread;
			page_frame_entry->v_address = vaddr_fault;
			page_frame_entry->dirty_bit = dbit;
			page_frame_entry->valid_bit = vbit;
			page_frame_entry->reference_bit = true;
			page_frame_entry->is_free = false;
			page_frame_entry->as = as;
			break;
		}

	}
	lock_release(vm_lock);
	KASSERT(i!=totpages);
	return 0;
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	paddr_t paddr;
	//kprintf("vm_fault called\n");
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	lock_acquire(vm_metrics_lock);
	VM_STATS->tlb_misses++;
	lock_release(vm_metrics_lock);

	faultaddress &= PAGE_FRAME;
	//kprintf("Virtual Memory: vm_fault : vaddr : %x\n",faultaddress);
	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	as = curthread->t_addrspace;
	
	if (as == NULL) {
		kprintf("Address space is NULL\n");
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}
	
	struct page_table* page_t = &(as->as_pagetable);
	int totpages = page_t->pt_totalpages;
	struct page_table_entry* pt_pointer = page_t->pt_entrytable;
	
	/* Assert that the address space has been set up properly. */
	KASSERT(totpages != 0);
	for(i=0;i<totpages;i++)
	{
		KASSERT(pt_pointer[i].vaddress!=0);
		KASSERT((pt_pointer[i].vaddress & PAGE_FRAME) == pt_pointer[i].vaddress);
		if(pt_pointer[i].vbit)
		{
			KASSERT(pt_pointer[i].paddress!=0);
			KASSERT((pt_pointer[i].paddress & PAGE_FRAME) == pt_pointer[i].paddress);
		}
	}
	//KASSERT(as->as_stackpbase!=0);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
	
	int id_thread = curthread->t_id;

	for (i = 0 ; i < totpages ; i++)
	{
		if (pt_pointer[i].vaddress == faultaddress)
		{
			if (pt_pointer[i].vbit == true)
			{
				/* Page already in memory, just set paddr */
				
				lock_acquire(vm_metrics_lock);
				VM_STATS->tlb_misses_with_page_in_memory++;
				lock_release(vm_metrics_lock);

				paddr = pt_pointer[i].paddress;
				pt_pointer[i].rbit = true;
				//kprintf("Virtual Memory: vm_fault : page already in memory. vadder: %x  paddr : %x\n",faultaddress,paddr);
				if (faulttype == VM_FAULT_WRITE) pt_pointer[i].dbit = true;
			}
			else
			{
				/* Code for Swapping in Page, which has been swapped out once, so pt_entry exists */
				
				paddr = getpage(1);
				//kprintf("Virtual Memory: vm_fault : page not in memory. vadder: %x  paddr : %x\n",faultaddress,paddr);
				int result = read_page_from_swap(paddr, faultaddress, as);
				if(result)
					return result;

				lock_acquire(vm_metrics_lock);
				VM_STATS->page_fault++;
				lock_release(vm_metrics_lock);

				/* Update the page table entry */
				pt_pointer[i].rbit = true;
				pt_pointer[i].vbit = true;
				pt_pointer[i].paddress = paddr;
				
				if (faulttype == VM_FAULT_WRITE) pt_pointer[i].dbit = true;
			}

			update_page_frame_entry(faultaddress, paddr, pt_pointer[i].vbit, pt_pointer[i].dbit,id_thread,as);

			break;
		}
	}
	if(i==totpages)
	kprintf("paddr : %x and faultaddress %x\n",paddr,faultaddress);
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
	
	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}



/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;

	}
	//kprintf("alloc_kpages returning paddress %x\n",PADDR_TO_KVADDR(pa));
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	/* nothing - leak the memory. */
	//kprintf("free_kpages called on vaddr : %x\n",addr);
	(void)addr;
}

void
vm_tlbshootdown_all(void)
{
	//panic("dumbvm tried to do tlb shootdown?!\n");
	int spl = splhigh();
	int i;
	
	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void reset_reference_bit(void)
{
	lock_acquire(vm_lock);
	struct vm_manager_page_entry* page_frame_entry;
	int totpages = VM->num_page_frames;
	int i;

	for (i = 0 ; i < totpages ; i++)
	{
		page_frame_entry = &(VM->page_frame_table[i]);
		page_frame_entry->reference_bit = false;
	}
	lock_release(vm_lock);
}
