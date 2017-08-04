#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <poll.h>

#include <xenctrl.h>
#include <xenevtchn.h>
#include <xen/vm_event.h>

#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#if defined(__arm__) || defined(__aarch64__)
#include <xen/arch-arm.h>
#define START_PFN (GUEST_RAM0_BASE >> 12)
#elif defined(__i386__) || defined(__x86_64__)
#define START_PFN 0ULL
#endif

#define DPRINTF(a, b...) fprintf(stderr, a, ## b)
#define ERROR(a, b...) fprintf(stderr, a "\n", ## b)
#define PERROR(a, b...) fprintf(stderr, a ": %s\n", ## b, strerror(errno))

/* From xen/include/asm-x86/processor.h */
#define X86_TRAP_DEBUG  1
#define X86_TRAP_INT3   3

typedef struct vm_event {
    domid_t domain_id;
    xenevtchn_handle *xce_handle;
    int port;
    vm_event_back_ring_t back_ring;
    uint32_t evtchn_port;
    void *ring_page;
} vm_event_t;

typedef struct xenaccess {
    xc_interface *xc_handle;

    xen_pfn_t max_gpfn;

    vm_event_t vm_event;
} xenaccess_t;


vm_event_request_t req;
vm_event_response_t rsp;

xc_interface *xch;
domid_t domain_id;
int required = 0;
int singlestep = 0;
int privcall = 0;
char* progname;
int shutting_down = 0;
int rc = 0;
static int interrupted;
xenaccess_t *xenaccess;

bool evtchn_bind = 0, evtchn_open = 0, mem_access_enable = 0;


int toggle_single_step_domain(int status)
{

    
    printf("Seeting Singlestep to %x\n", status);

    rc = xc_monitor_singlestep( xch, domain_id, status );
        
    if ( rc < 0 && status)
    {
        ERROR("Error %d failed to enable singlestep monitoring!\n", rc);
        return rc;
    }else
    {
        if(!status)
        {
            printf("Singlestep deactivated\n" );
        }
    }
    printf("SingleStep succesfull rc=%d\n", rc);
    return rc;

}

int xenaccess_teardown(xc_interface *xch, xenaccess_t *xenaccess)
{
    int rc;

    if ( xenaccess == NULL )
        return 0;

    /* Tear down domain xenaccess in Xen */
    if ( xenaccess->vm_event.ring_page )
        munmap(xenaccess->vm_event.ring_page, XC_PAGE_SIZE);

    if ( mem_access_enable )
    {
        rc = xc_monitor_disable(xenaccess->xc_handle,
                                xenaccess->vm_event.domain_id);
        if ( rc != 0 )
        {
            ERROR("Error tearing down domain xenaccess in xen");
            return rc;
        }
    }

    /* Unbind VIRQ */
    if ( evtchn_bind )
    {
        rc = xenevtchn_unbind(xenaccess->vm_event.xce_handle,
                              xenaccess->vm_event.port);
        if ( rc != 0 )
        {
            ERROR("Error unbinding event port");
            return rc;
        }
    }

    /* Close event channel */
    if ( evtchn_open )
    {
        rc = xenevtchn_close(xenaccess->vm_event.xce_handle);
        if ( rc != 0 )
        {
            ERROR("Error closing event channel");
            return rc;
        }
    }

    /* Close connection to Xen */
    rc = xc_interface_close(xenaccess->xc_handle);
    if ( rc != 0 )
    {
        ERROR("Error closing connection to xen");
        return rc;
    }
    xenaccess->xc_handle = NULL;

    free(xenaccess);

    return 0;
}



xenaccess_t *xenaccess_init(xc_interface **xch_r, domid_t domain_id)
{
    xenaccess_t *xenaccess = 0;
    xc_interface *xch;
    int rc;

    xch = xc_interface_open(NULL, NULL, 0);
    if ( !xch )
        goto err_iface;

    printf("xenaccess init\n");
    *xch_r = xch;

    /* Allocate memory */
    xenaccess = malloc(sizeof(xenaccess_t));
    memset(xenaccess, 0, sizeof(xenaccess_t));

    /* Open connection to xen */
    xenaccess->xc_handle = xch;

    /* Set domain id */
    xenaccess->vm_event.domain_id = domain_id;

    /* Enable mem_access */
    xenaccess->vm_event.ring_page =
            xc_monitor_enable(xenaccess->xc_handle,
                              xenaccess->vm_event.domain_id,
                              &xenaccess->vm_event.evtchn_port);
    if ( xenaccess->vm_event.ring_page == NULL )
    {
        switch ( errno ) {
            case EBUSY:
                ERROR("xenaccess is (or was) active on this domain");
                break;
            case ENODEV:
                ERROR("EPT not supported for this guest");
                break;
            default:
                perror("Error enabling mem_access");
                break;
        }
        goto err;
    }
    mem_access_enable = 1;

    /* Open event channel */
    xenaccess->vm_event.xce_handle = xenevtchn_open(NULL, 0);
    if ( xenaccess->vm_event.xce_handle == NULL )
    {
        ERROR("Failed to open event channel");
        goto err;
    }
    evtchn_open = 1;

    /* Bind event notification */
    rc = xenevtchn_bind_interdomain(xenaccess->vm_event.xce_handle,
                                    xenaccess->vm_event.domain_id,
                                    xenaccess->vm_event.evtchn_port);
    if ( rc < 0 )
    {
        ERROR("Failed to bind event channel");
        goto err;
    }
    evtchn_bind = 1;
    xenaccess->vm_event.port = rc;

    /* Initialise ring */
    SHARED_RING_INIT((vm_event_sring_t *)xenaccess->vm_event.ring_page);
    BACK_RING_INIT(&xenaccess->vm_event.back_ring,
                   (vm_event_sring_t *)xenaccess->vm_event.ring_page,
                   XC_PAGE_SIZE);

    /* Get max_gpfn */
    rc = xc_domain_maximum_gpfn(xenaccess->xc_handle,
                                xenaccess->vm_event.domain_id,
                                &xenaccess->max_gpfn);

    if ( rc )
    {
        ERROR("Failed to get max gpfn");
        goto err;
    }

    DPRINTF("max_gpfn = %"PRI_xen_pfn"\n", xenaccess->max_gpfn);

    return xenaccess;

 err:
    rc = xenaccess_teardown(xch, xenaccess);
    if ( rc )
    {
        ERROR("Failed to teardown xenaccess structure!\n");
    }

 err_iface:
    return NULL;
}


void usage(char* progname)
{
    fprintf(stderr, "Usage: %s [-m] <domain_id> write|exec", progname);
#if defined(__i386__) || defined(__x86_64__)
            fprintf(stderr, "|breakpoint|altp2m_write|altp2m_exec|debug|cpuid|desc_access");
#elif defined(__arm__) || defined(__aarch64__)
            fprintf(stderr, "|privcall");
#endif
            fprintf(stderr,
            "\n"
            "Logs first page writes, execs, or breakpoint traps that occur on the domain.\n"
            "\n"
            "-m requires this program to run, or else the domain may pause\n");
}

static void get_request(vm_event_t *vm_event, vm_event_request_t *req)
{
    vm_event_back_ring_t *back_ring;
    RING_IDX req_cons;

    back_ring = &vm_event->back_ring;
    req_cons = back_ring->req_cons;

    /* Copy request */
    memcpy(req, RING_GET_REQUEST(back_ring, req_cons), sizeof(*req));
    req_cons++;

    /* Update ring */
    back_ring->req_cons = req_cons;
    back_ring->sring->req_event = req_cons + 1;
}


int xc_wait_for_event_or_timeout(xc_interface *xch, xenevtchn_handle *xce, unsigned long ms)
{
    struct pollfd fd = { .fd = xenevtchn_fd(xce), .events = POLLIN | POLLERR };
    int port;
    int rc;

    rc = poll(&fd, 1, ms);
    if ( rc == -1 )
    {
        if (errno == EINTR)
            return 0;

        ERROR("Poll exited with an error");
        goto err;
    }

    if ( rc == 1 )
    {
        port = xenevtchn_pending(xce);
        if ( port == -1 )
        {
            ERROR("Failed to read port from event channel");
            goto err;
        }

        rc = xenevtchn_unmask(xce, port);
        if ( rc != 0 )
        {
            ERROR("Failed to unmask event channel port");
            goto err;
        }
    }
    else
        port = -1;

    return port;

 err:
    return -errno;
}

static void put_response(vm_event_t *vm_event, vm_event_response_t *rsp)
{
    vm_event_back_ring_t *back_ring;
    RING_IDX rsp_prod;

    back_ring = &vm_event->back_ring;
    rsp_prod = back_ring->rsp_prod_pvt;

    /* Copy response */
    memcpy(RING_GET_RESPONSE(back_ring, rsp_prod), rsp, sizeof(*rsp));
    rsp_prod++;

    /* Update ring */
    back_ring->rsp_prod_pvt = rsp_prod;
    RING_PUSH_RESPONSES(back_ring);
}
void intHandler(int dummy)
{
    int rc1;
    printf("\nCatched CTRL+D\n" );
    toggle_single_step_domain(0);
     rc1 = xenaccess_teardown(xch, xenaccess);
    if ( rc1 != 0 )
        ERROR("Error tearing down xenaccess");

    printf("RIP xenaccess\n" );

    exit(0);
}
void event_loop(void)
{
    
    char with_commands;
    int rc,rc1;
    int run = 0;
    int ss_started = 0;
    char command;

    xenaccess = xenaccess_init(&xch, domain_id);
    if ( xenaccess == NULL )
    {
        ERROR("Error initialising xenaccess");
        return ;
    }


    rc = xc_monitor_privileged_call(xch, domain_id, 1);
    if ( rc < 0 )
    {
        ERROR("Error %d setting privileged call trapping with vm_event\n", rc);
        return;
    }
       
    
    //toggle_single_step_domain(1);
    printf("Run singlestep with commands? (y/n):");
    do
    {
        with_commands = getchar();
    }while (isspace(with_commands));

    if(with_commands == 'n')
    {
        toggle_single_step_domain(1);
        run = 1;    
    }

    signal(SIGINT, intHandler);

    printf("Starting loop\n" );
    for (;;)
    {
        if ( interrupted )
        {
            /* Unregister for every event */
            DPRINTF("xenaccess shutting down on signal %d\n", interrupted);

            //rc = xc_monitor_privileged_call(xch, domain_id, 0);
            DPRINTF("Deactivating single step\n");
            toggle_single_step_domain(0);
            

            shutting_down = 1;
        }

        rc = xc_wait_for_event_or_timeout(xch, xenaccess->vm_event.xce_handle, 100);
        if ( rc < -1 )
        {
            ERROR("Error getting event");
            interrupted = -1;
            continue;
        }
        else if ( rc != -1 )
        {
            //DPRINTF("Got event from Xen\n");
        }

        while ( RING_HAS_UNCONSUMED_REQUESTS(&xenaccess->vm_event.back_ring) )
        {
            get_request(&xenaccess->vm_event, &req);

            if ( req.version != VM_EVENT_INTERFACE_VERSION )
            {
                ERROR("Error: vm_event interface version mismatch!\n");
                interrupted = -1;
                continue;
            }

            memset( &rsp, 0, sizeof (rsp) );
            rsp.version = VM_EVENT_INTERFACE_VERSION;
            rsp.vcpu_id = req.vcpu_id;
            rsp.flags = (req.flags & VM_EVENT_FLAG_VCPU_PAUSED);
            rsp.reason = req.reason;

            switch (req.reason) {
            case VM_EVENT_REASON_PRIVILEGED_CALL:
                printf("++++++++++++++++++++++++++++Privileged call: pc=%"PRIx64" (vcpu %d)\n",
                       req.data.regs.arm.pc,
                       req.vcpu_id);


                if (!ss_started)
                {
                    ss_started = 1;
                    toggle_single_step_domain(1);
                }else
                {
                    printf("Signlestep already activated\n");
                }
            

                
                rsp.data.regs.arm = req.data.regs.arm;
                rsp.data.regs.arm.pc += 4;
                rsp.flags |= VM_EVENT_FLAG_SET_REGISTERS;
                break;
            case VM_EVENT_REASON_SINGLESTEP:


                //printf("#### Singlestep: PC=%016"PRIx64", vcpu %d\n", req.data.regs.arm.pc,req.vcpu_id);

                printf("Singlestep: PC=%016"PRIx64", vcpu %d, \n", req.data.regs.arm.pc,req.vcpu_id);

                       /*req.data.regs.x86.rip,
                       req.vcpu_id,
                       req.altp2m_idx);
                        */
                
                if(!run)
                {

                    
                    
                    printf("Enter command: s:step; c:end; r:regs:;x:end :" );
                    //scanf("%c", &command);

                    do
                    {
                        command = getchar();
                    }
                    while (isspace(command));


                    switch(command)
                    {
                        case 'c':   printf("Singlestep weiterlaufen \n");
                                   // rsp.flags |= VM_EVENT_FLAG_TOGGLE_SINGLESTEP;
                                    run = 1;
                                    break;
                        case 's':
                                    break;
                        case 'r':   printf("################################\n");
                                    printf("Register Values\n");
                                    printf("ttbr0           0x%lx\n",req.data.regs.arm.ttbr0 );
                                    printf("ttbr1           0x%lx\n",req.data.regs.arm.ttbr1 );
                                    printf("ttbcr           0x%lx\n",req.data.regs.arm.ttbcr );
                                    printf("pc              0x%lx\n",req.data.regs.arm.pc );
                                    printf("CPSR (SPSR)     0x%x\n",req.data.regs.arm.cpsr );
                                    printf("Singlestep->gfn 0x%lx\n", req.u.singlestep.gfn);
                                    printf("_pad            0x%x\n",req.data.regs.arm._pad );
                                    printf("################################\n");

                                    break;
                        case 'x':   printf("Singlestep beenden \n");
                                    rsp.flags |= VM_EVENT_FLAG_TOGGLE_SINGLESTEP;
                                    run = 1;
                                    interrupted = -1;
                                    break;
                        default:    printf("Wrong command \"%c\"!\n", command );
                                    break;

                    }

                    
                   

                  }
                  
                    //toggle_single_step_domain(1);
                    //printf("Privileged call von module \n" );
        
                //rsp.flags |= VM_EVENT_FLAG_TOGGLE_SINGLESTEP;

                break;
            default:
                fprintf(stderr, "UNKNOWN REASON CODE %d\n", req.reason);
            }

            /* Put the response on the ring */
            put_response(&xenaccess->vm_event, &rsp);
        }

        /* Tell Xen page is ready */
        rc = xenevtchn_notify(xenaccess->vm_event.xce_handle,
                              xenaccess->vm_event.port);

        if ( rc != 0 )
        {
            ERROR("Error resuming page");
            interrupted = -1;
        }

        if ( shutting_down )
            break;
    }


     /* Tear down domain xenaccess */
    rc1 = xenaccess_teardown(xch, xenaccess);
    if ( rc1 != 0 )
        ERROR("Error tearing down xenaccess");

    if ( rc == 0 )
        rc = rc1;

    DPRINTF("xenaccess exit code %d\n", rc);
    

    
    
}

int main(int argc, char *argv[])
{
    int required = 0;

    /*BA part*/
    int singlestep = 0;


    progname = argv[0];
    argv++;
    argc--;

    if ( argc == 3 && argv[0][0] == '-' )
    {
        if ( !strcmp(argv[0], "-m") )
            required = 1;
        else
        {
            usage(progname);
            return -1;
        }
        argv++;
        argc--;
    }

    if ( argc != 2 )
    {
        usage(progname);
        return -1;
    }

    domain_id = atoi(argv[0]);
    argv++;
    argc--;

    if ( !strcmp(argv[0], "privcall") )
    {
        privcall = 1;
    }

    if ( !strcmp(argv[0], "singlestep") )
    {
        /*BA part*/
        printf("Singlestep request found\n");
        singlestep = 1;
    }

    event_loop();

}