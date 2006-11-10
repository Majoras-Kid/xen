
#ifndef __XEN_DOMAIN_H__
#define __XEN_DOMAIN_H__

struct vcpu *alloc_vcpu(
    struct domain *d, unsigned int vcpu_id, unsigned int cpu_id);
int boot_vcpu(
    struct domain *d, int vcpuid, struct vcpu_guest_context *ctxt);
struct vcpu *alloc_idle_vcpu(unsigned int cpu_id);

struct domain *alloc_domain(domid_t domid);
void free_domain(struct domain *d);

/*
 * Arch-specifics.
 */

/* Allocate/free a VCPU structure. */
struct vcpu *alloc_vcpu_struct(void);
void free_vcpu_struct(struct vcpu *v);

/*
 * Initialise/destroy arch-specific details of a VCPU.
 *  - vcpu_initialise() is called after the basic generic fields of the
 *    VCPU structure are initialised. Many operations can be applied to the
 *    VCPU at this point (e.g., vcpu_pause()).
 *  - vcpu_destroy() is called only if vcpu_initialise() previously succeeded.
 */
int  vcpu_initialise(struct vcpu *v);
void vcpu_destroy(struct vcpu *v);

int arch_domain_create(struct domain *d);

void arch_domain_destroy(struct domain *d);

int arch_set_info_guest(struct vcpu *v, struct vcpu_guest_context *c);

void domain_relinquish_resources(struct domain *d);

void dump_pageframe_info(struct domain *d);

void arch_dump_vcpu_info(struct vcpu *v);

void arch_dump_domain_info(struct domain *d);

#endif /* __XEN_DOMAIN_H__ */
