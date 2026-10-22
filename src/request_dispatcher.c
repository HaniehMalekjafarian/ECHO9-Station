#include "request_dispatcher.h"
#include "agentos_runtime.h"
#include "backend.h"

/* Only validated protocol-v1 infer requests reach this instructor-provided
 * integration point. PCB lifecycle is owned by the student ProcessManager. */
agentos_dispatch_result_t request_dispatch_infer(
    const agentos_infer_request_t *request,char *answer,size_t capacity)
{
    if(!request||!request->agent_id||!request->request_id||!request->prompt||
       !answer||capacity==0)return AGENTOS_DISPATCH_BACKEND_ERROR;
    agentos_pm_t *pm=agentos_runtime_pm();
    if(!pm)return AGENTOS_DISPATCH_BACKEND_ERROR;
    pm_snapshot_t p;
    pm_result_t registration=pm_auto_register(pm,request->agent_id,&p,NULL);
    switch(registration){
      case PM_OK:break;
      case PM_DISABLED:return AGENTOS_DISPATCH_DISABLED;
      case PM_ALREADY_TERMINATED:return AGENTOS_DISPATCH_TERMINATED;
      case PM_FULL:return AGENTOS_DISPATCH_TABLE_FULL;
      default:return AGENTOS_DISPATCH_BACKEND_ERROR;
    }
    agentos_runtime_request_begin(request->agent_id,p.pid);
    agentos_runtime_inference_begin(p.pid);
    int backend_result=backend_infer(request->prompt,answer,capacity);
    agentos_runtime_inference_end(p.pid);
    agentos_runtime_request_end(p.pid);
    return backend_result==0?AGENTOS_DISPATCH_OK:AGENTOS_DISPATCH_BACKEND_ERROR;
}
