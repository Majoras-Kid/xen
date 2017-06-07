/*
 * arch/arm/monitor.c
 *
 * Arch-specific monitor_op domctl handler.
 *
 * Copyright (c) 2016 Tamas K Lengyel (tamas.lengyel@zentific.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/vm_event.h>
#include <xen/monitor.h>
#include <asm/monitor.h>
#include <asm/vm_event.h>
#include <public/vm_event.h>

int arch_monitor_domctl_event(struct domain *d,
                              struct xen_domctl_monitor_op *mop)
{
    struct arch_domain *ad = &d->arch;
    int i =0;
    bool_t requested_status = (XEN_DOMCTL_MONITOR_OP_ENABLE == mop->op);

    switch ( mop->event )
    {
    case XEN_DOMCTL_MONITOR_EVENT_PRIVILEGED_CALL:
    {
        bool_t old_status = ad->monitor.privileged_call_enabled;

        if ( unlikely(old_status == requested_status) )
            return -EEXIST;

        domain_pause(d);
        ad->monitor.privileged_call_enabled = requested_status;
        domain_unpause(d);
        break;
    }

    case XEN_DOMCTL_MONITOR_EVENT_SINGLESTEP:
    {
        //Set Debug to Linked Addres
        //See AARM C3.3.7 Linked comparisons for [...]
        
        //Example on ARM ARM 2051

        gprintk(XENLOG_ERR, "Setup HypTrap Route done\n");
        gprintk(XENLOG_ERR, "[Before] Reading DBGBCR2:   0x%x\n", READ_SYSREG( p14,0,c0,c2,5));
        //gprintk(XENLOG_ERR, "[Before] Reading DBGBCR3:   0x%x\n", READ_SYSREG( p14,0,c0,c3,5));
        gprintk(XENLOG_ERR, "[Before] Reading DBGBVR:    0x%x\n", READ_SYSREG( p14,0,c0,c2,4));
        gprintk(XENLOG_ERR, "[Before] Reading DBGDSCREXT:0x%x\n", READ_SYSREG(DBGDSCREXT));
         
        
        //Route Exceptions to Hypervisor
        //Set: HDCR_{TDE} + init_traps()
       // WRITE_SYSREG((vaddr_t)hyp_traps_vector, VBAR_EL2);
        WRITE_SYSREG(READ_SYSREG(MDCR_EL2) | HDCR_TDRA|HDCR_TDOSA|HDCR_TDA|HDCR_TDE, MDCR_EL2);


        //DBGBCR2 =  (p14,0,c0,c2,5)== Unliked Address Mismatch: 0b0100==0x404007 
        //(linked: 0b0101) ->
        //PCM: Bit 1,2   -> Value=0b11 -> PL0/PL1
        //HCM: Bit 13    -> Value=0b00 -> No HypMode Trap
        //SSC: Bit 14/15 -> Value 0b01 -> NonSecure only
        //BAS: ARM + Address + BAS=0b0000 -> Mismatch Hit (2047)
        //Prefetch Abort Eception -> Anhand von (2037)
        //Mask und ByteAddressSelect nicht gesetzt
        // Res  mask   BT    LBN    SSC  HCM  SBZP   BAS    RES  PMC  E
        // 000  00000  0101  0011   01   0    0000   0000   00   11   1 = 0x534007 
        // 000  00000  0100  0000   01   0    0000   0000   00   11   1 = 0x404007
        WRITE_SYSREG(0x404007,  p14,0,c0,c2,5);
           

        //BVR: Breakpoint value register
        // TODO: 1³² or 0³² as BVR1 Address?
        // Instruction Address            Res
        // 000000000000000000000000000000 00
        //trying 20 as address
        //DBGBVR0 = p14,0,c0,c2,4
        WRITE_SYSREG(0xB,p14,0,c0,c2,4 );


        /*
        //Not implemented yet, bc. Linking not working
        //DBGBCR3 =  (p14,0,c0,c3,5) == Linked VMID match: 0b1001
        // TODO: Check if same SSC, HCM, PMC as Addres Mismatch?
        // Res  mask   BT    LBN    SSC  HCM  SBZP   BAS    RES  PMC  E
        //neue Maske
        // 000  00000  1010  0000   00   0    0000   1111   00   11   1 = 0xA001E7
        */
        //WRITE_SYSREG(0xA001E7, p14,0,c0,c3,5);
         

        //DBGBXVR = p14,0,c1,c3,1 = : VMID
        // Reserved                 VMID
        // 000000000000000000000000 00000001
        //WRITE_SYSREG(1, p14,0,c1,c3,1); 
       

        //DBGDSCR = Enable Invasive Debug + Monitor Mode
        //Access 2251
        //TODO Authentication signals not correct
        //MDBGen[15] = 1
        //MOE[5:2] = 0b0001
        //DBGack[10] = 1 DebugAcknowledge -> forced BP to give signal (Read as UNpredigtable)
        //0000 0000 0000 0000 0100 0100 00 0001 00 = 0x4004 (mit DBGACK=0x4404) 
        //     0010 0000 0100 0100 0000 00 0001 10
        //PipeADV[25] = Piping instruction (READONLY)
        //NS[18]: CPU in Non-Secure (READONLY)  
        //RESTARTED[1]: CPU exited Debug state (READONLY)    READ_AS_ONE 
        WRITE_SYSREG(0x4404,DBGDSCREXT);


        for (i = 0; i < 10000000; ++i)
        {
            /* just for delay */
        }

        gprintk(XENLOG_ERR, "[After] Reading HDCR:      0x%x\n", READ_SYSREG( MDCR_EL2));
        gprintk(XENLOG_ERR, "[After] Reading DBGBCR2:   0x%x\n", READ_SYSREG( p14,0,c0,c2,5));
        //gprintk(XENLOG_ERR, "[After] Reading DBGBCR3:   0x%x\n", READ_SYSREG( p14,0,c0,c3,5));
        gprintk(XENLOG_ERR, "[Before] Reading DBGBVR:    0x%x\n", READ_SYSREG( p14,0,c0,c2,4));
        gprintk(XENLOG_ERR, "[After] Reading DBGDSCREXT:0x%x\n", READ_SYSREG(DBGDSCREXT));
        
        //gprintk(XENLOG_GUEST, "[After] Reading DBGBXVR: 0x%x\n", READ_SYSREG( p14,0,c1,c3,1));

         return 0;
          /*Fallthrough, bc. test print*/
    }
    default:
        /*
         * Should not be reached unless arch_monitor_get_capabilities() is
         * not properly implemented.
         */
        ASSERT_UNREACHABLE();
        return -EOPNOTSUPP;
    }

    return 0;
}

int monitor_smc(void)
{
    vm_event_request_t req = {
        .reason = VM_EVENT_REASON_PRIVILEGED_CALL
    };

    return monitor_traps(current, 1, &req);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
