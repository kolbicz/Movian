/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <stdint.h>

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include <mach/mach.h>
#include <mach/task.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/task_info.h>

#include "main.h"
#include "misc/callout.h"
#include "prop/prop.h"

static prop_t *p_sys;
static unsigned int *last_total;
static unsigned int *last_idle;
static unsigned int cpu_count;
static callout_t timer;
static prop_t *p_cpuroot;
static prop_t **p_cpu;
static prop_t **p_load;




static void
mem_monitor_do(void)
{
  uint64_t physical_memory;
  size_t length = sizeof(physical_memory);
  if(sysctlbyname("hw.memsize", &physical_memory, &length, NULL, 0) < 0)
    return;

  vm_size_t pagesize;
  if(host_page_size(mach_host_self(), &pagesize) != KERN_SUCCESS)
    return;

  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

  vm_statistics64_data_t vmstat;
  if(host_statistics64(mach_host_self(), HOST_VM_INFO64,
                       (host_info64_t)&vmstat, &count) != KERN_SUCCESS)
    return;

  const uint64_t system_available =
    ((uint64_t)vmstat.free_count +
     (uint64_t)vmstat.inactive_count +
     (uint64_t)vmstat.speculative_count) * pagesize;

#if TARGET_OS_IPHONE
  // This is the useful allocation budget on iOS: the number of bytes the
  // process may dirty before reaching its current Jetsam memory limit.
  uint64_t process_available;
  if(__builtin_available(iOS 13.0, tvOS 13.0, *))
    process_available = os_proc_available_memory();
  else
    process_available = system_available;
#else
  // macOS has no per-process dirty-memory limit API. Reclaimable system RAM is
  // the closest equivalent and is also the appropriate backend budget here.
  const uint64_t process_available = system_available;
#endif

  prop_t *mem = prop_create(p_sys, "mem");

  prop_set(mem, "systotal", PROP_SET_INT,
           (int)(physical_memory / (1024 * 1024)));
  prop_set(mem, "sysfree", PROP_SET_INT,
           (int)(system_available / (1024 * 1024)));
  prop_set(mem, "sysfreeA", PROP_SET_INT,
           (int)(process_available / (1024 * 1024)));

  // HLS and BitTorrent account this value in bytes. Their allocations adjust
  // it between monitor ticks, after which this snapshot refreshes the budget.
  gconf.android_free_mem = process_available;

  task_vm_info_data_t info;
  mach_msg_type_number_t size = TASK_VM_INFO_COUNT;
  if(task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &size) ==
     KERN_SUCCESS) {
    prop_set(mem, "activeMem", PROP_SET_INT,
             (int)(info.phys_footprint / (1024 * 1024)));
  }
}




static void
cpu_monitor_do(void)
{
  kern_return_t r;
  processor_info_t pinfo;
  mach_msg_type_number_t msg_count;
  unsigned int cpu_count_temp;
  char buf[100];
 
  r = host_processor_info(mach_host_self (),
                          PROCESSOR_CPU_LOAD_INFO,
                          &cpu_count_temp,
                          (processor_info_array_t *)&pinfo,
                          &msg_count);
  if(r != KERN_SUCCESS)
    return;
  
  if(cpu_count != cpu_count_temp) {
    vm_deallocate(mach_task_self(),
                  (vm_address_t)pinfo,
                  (vm_size_t)sizeof(*pinfo) * msg_count);
    return;
  }
  
  int i;
  for(i = 0; i < cpu_count; i++) {
    if(p_cpu[i] == NULL) {
      p_cpu[i] = prop_create(p_cpuroot, NULL);
      
      snprintf(buf, sizeof(buf), "CPU%d", i);
      prop_set_string(prop_create(p_cpu[i], "name"), buf);
      p_load[i] = prop_create(p_cpu[i], "load");      
    }
    
    processor_info_t pi = pinfo + (CPU_STATE_MAX * i);
    
    unsigned int total = (pi[CPU_STATE_USER] +
                          pi[CPU_STATE_SYSTEM] +
                          pi[CPU_STATE_NICE] +
                          pi[CPU_STATE_IDLE]);
    unsigned int idle = pi[CPU_STATE_IDLE];
    
    unsigned int di = idle - last_idle[i];
    unsigned int dt = total - last_total[i];
    last_idle[i] = idle;
    last_total[i] = total;
    
    prop_set_float(p_load[i], 1.0 - ((float)di / (float)dt));
  }
  
  vm_deallocate(mach_task_self(),
                (vm_address_t)pinfo,
                (vm_size_t)sizeof(*pinfo) * msg_count);
}

static void 
timercb(callout_t *c, void *aux)
{
  callout_arm(&timer, timercb, NULL, 1);
  cpu_monitor_do();
  mem_monitor_do();
}

static void
darwin_init_cpu_monitor(void)
{
  kern_return_t r;
  processor_info_t pinfo;
  mach_msg_type_number_t msg_count;

  p_sys = prop_create(prop_get_global(), "system");

  r = host_processor_info(mach_host_self (),
                          PROCESSOR_CPU_LOAD_INFO,
                          &cpu_count,
                          (processor_info_array_t *)&pinfo,
                          &msg_count);
  if(r != KERN_SUCCESS) {
    TRACE(TRACE_ERROR, "darwin",
          "host_processor_info(PROCESSOR_CPU_LOAD_INFO) failed %d", r);
    return;
  }
  
  p_cpu = calloc(cpu_count, sizeof(prop_t *));
  p_load  = calloc(cpu_count, sizeof(prop_t *));
  last_total = calloc(cpu_count, sizeof(unsigned int));
  last_idle = calloc(cpu_count, sizeof(unsigned int));
  
  prop_set_int(prop_create(prop_create(p_sys, "cpuinfo"), "available"), 1);
  p_cpuroot =  prop_create(prop_create(p_sys, "cpuinfo"), "cpus");
  
  vm_deallocate(mach_task_self(),
                (vm_address_t)pinfo,
                (vm_size_t)sizeof(*pinfo) * msg_count);
  
  cpu_monitor_do();
  mem_monitor_do();
  callout_arm(&timer, timercb, NULL, 1);
}

INITME(INIT_GROUP_API, darwin_init_cpu_monitor, NULL, 0);
