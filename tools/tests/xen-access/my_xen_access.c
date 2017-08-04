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

void event_loop(void)
{
	for (;;)
    {
        if ( interrupted )
        {
            /* Unregister for every event */
            DPRINTF("xenaccess shutting down on signal %d\n", interrupted);

            if ( singlestep )
            {
                 DPRINTF("Deactivating single step\n");
                rc = xc_monitor_singlestep(xch, domain_id, 0);
            }

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
            DPRINTF("Got event from Xen\n");
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
                printf("Privileged call: pc=%"PRIx64" (vcpu %d)\n",
                       req.data.regs.arm.pc,
                       req.vcpu_id);


                if( singlestep )
                {
                    printf("Privileged call von module \n" );
                }
                rsp.data.regs.arm = req.data.regs.arm;
                rsp.data.regs.arm.pc += 4;
                rsp.flags |= VM_EVENT_FLAG_SET_REGISTERS;
                break;
            case VM_EVENT_REASON_SINGLESTEP:
                //printf("Singlestep: rip=%016"PRIx64", vcpu %d, altp2m %u\n", req.data.regs.arm.pc,req.vcpu_id,0);
                       /*req.data.regs.x86.rip,
                       req.vcpu_id,
                       req.altp2m_idx);
                        */
                if ( altp2m )
                {
                    printf("\tSwitching altp2m to view %u!\n", altp2m_view_id);

                    rsp.flags |= VM_EVENT_FLAG_ALTERNATE_P2M;
                    rsp.altp2m_idx = altp2m_view_id;
                }

                if(singlestep_counter >=10)
                {
                    rsp.flags |= VM_EVENT_FLAG_TOGGLE_SINGLESTEP;
                }
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
}

int toggle_single_step_domain(int status)
{

    
    DPRINTF("Seeting Singlestep to %x\n", status);

    rc = xc_monitor_singlestep( xch, domain_id, status );
        
    if ( rc < 0 )
    {
        ERROR("Error %d failed to enable singlestep monitoring!\n", rc);
        return rc;
    }
    DPRINTF("SingleStep succesfull rc=%d\n", rc);
   	return rc;

}

int main(int argc, char *argv[])
{
	struct sigaction act;
    domid_t domain_id;
    xenaccess_t *xenaccess;
    vm_event_request_t req;
    vm_event_response_t rsp;
    int rc = -1;
    int rc1;
    xc_interface *xch;
    xenmem_access_t default_access = XENMEM_access_rwx;
    xenmem_access_t after_first_access = XENMEM_access_rwx;
    int memaccess = 0;
    int required = 0;
    int breakpoint = 0;
    int shutting_down = 0;
    int privcall = 0;
    int altp2m = 0;
    int debug = 0;
    int cpuid = 0;
    int desc_access = 0;
    uint16_t altp2m_view_id = 0;



	/*BA part*/
    int singlestep = 0;


    char* progname = argv[0];
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



    if ( !strcmp(argv[0], "singlestep") )
    {
        /*BA part*/
        singlestep = 1;
    }




}