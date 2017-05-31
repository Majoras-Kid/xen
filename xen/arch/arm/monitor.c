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
    bool_t requested_status = (XEN_DOMCTL_MONITOR_OP_ENABLE == mop->op);

    gprintk(XENLOG_GUEST,"in arch_monitor_domctl_event with ");

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
       gprintk(XENLOG_GUEST,"Found SingleStep Request on ARM\n");
       //Route Exceptions to Hypervisor
       //Set: HDCR_{TDE} + init_traps()
       init_traps();
       WRITE_SYSREG(HDCR_TDE, MDCR_EL2);

       gprintk(XENLOG_GUEST, "Setup HypTrap Route done\n");
       gprintk(XENLOG_GUEST, "Reading DBGBCR0: %d\n", READ_SYSREG(DBGBCR0));
       gprintk(XENLOG_GUEST, "Reading DBGBCR1: %d\n", READ_SYSREG(DBGBCR1));
       gprintk(XENLOG_GUEST, "Reading DBGBVR:  %d\n", READ_SYSREG(DBGBVR1));
       
       //WRITE_SYSREG(READ_SYSREG(DBGBCR0)| 1, DBGBCR0);
      // WRITE_SYSREG(READ_SYSREG(DBGBCR1)| 1, DBGBCR1);

       
       //Set Debug to Linked Addres
       // See AARM C3.3.7 Linked comparisons for [...]
      
  
       //DBGBCR0 == Linked VMID match: 0b1001
       // TODO: Check if same SSC, HCM, PMC as Addres Mismatch?
       // Res  mask   BT    LBN    SSC  HCM  SBZP   BAS    RES  PMC  E
       // 000  00000  1001  0001   01   0    0000   0000   00   11   1
       //mask = 158613142241329;
        WRITE_SYSREG(0b100100010100000000000111, DBGBCR0);

       //DBGBXVR: VMID
       // Reserved                 VMID
       // 000000000000000000000000 00000000
        
        //TODO: Write VMID to DBGBXVR Register. Maybe with ASM?
        //WRITE_SYSREG32(1, DBGBXVR);




       //DBGBCR1 == Unliked Address Mismatch: 0b0100 (linked: 0b0101)
       //PCM: Bit 1,0   -> Value=0b11 -> PL0/PL1 only
       //HCM: Bit 13    -> Value=0b00 -> No HypMode Trap
       //SSC: Bit 14/15 -> Value 0b01 -> NonSecure only

       // Res  mask   BT    LBN    SSC  HCM  SBZP   BAS    RES  PMC  E
       // 000  00000  0100  0000   01   0    0000   0000   00   11   1
        WRITE_SYSREG(0b010000000100000000000111, DBGBCR1);


       //BVR: Breakpoint value register
       // TODO: 1³² or 0³² as BVR1 Address?
       // Instruction Address            Res
       // 000000000000000000000000000000 00
        WRITE_SYSREG(0, DBGBVR1);


      

       gprintk(XENLOG_GUEST, "Reading DBGBCR0: %db\n", READ_SYSREG(DBGBCR0));
       gprintk(XENLOG_GUEST, "Reading DBGBCR1: %db\n", READ_SYSREG(DBGBCR1));
       gprintk(XENLOG_GUEST, "Reading DBGBVR:  %db\n", READ_SYSREG(DBGBVR1));
       

       ASSERT_UNREACHABLE();
       return -EOPNOTSUPP;
        /*Fallthrough, bc. test print*/

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
