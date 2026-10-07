#include "okl_nxp.h"
#include <string.h>

typedef struct { uint8_t cls, op, size; } descriptor;
static const descriptor commands[OKL_COMMAND_COUNT] = {
    {0,0x87,0}, {0,0x84,0}, {0,0xc9,0},
    {15,0x82,2}, {15,0x84,2}, {3,0x83,3}, {3,0x81,2},
    {0,0x49,72}, {15,2,12}, {15,4,3}, {3,3,4}, {3,1,4}, {15,3,9},
    {0,0xfe,0}, {0,0xfc,0}, {0,0xfd,4}
};

static int all_zero(const uint8_t *bytes, size_t size) {
    size_t i;
    for(i=0;i<size;++i) if(bytes[i]) return 0;
    return 1;
}

static int known_effect(uint8_t effect) {
    return effect<=4 || effect==7 || effect==8 || effect==12;
}

static okl_result valid_request(const okl_request *r) {
    const uint8_t *a;
    uint16_t temperature;
    if(!r || (unsigned)r->command>=OKL_COMMAND_COUNT) return OKL_INVALID;
    if(r->size!=commands[r->command].size) return OKL_INVALID;
    a=r->arguments;
    switch(r->command) {
    case OKL_GET_EFFECT: case OKL_GET_COLOR_BRIGHTNESS:
        return all_zero(a,2)?OKL_OK:OKL_INVALID;
    case OKL_GET_WHITE_BRIGHTNESS:
        return a[0]==0 && a[1]==32 && a[2]==0?OKL_OK:OKL_INVALID;
    case OKL_GET_TEMPERATURE:
        return a[0]==0 && a[1]==32?OKL_OK:OKL_INVALID;
    case OKL_SET_OWNER:
        if(a[0]>1 || a[7]>64 || !all_zero(a+8+a[7],64-a[7])) return OKL_INVALID;
        return OKL_OK;
    case OKL_SET_EFFECT:
        if(a[0] || a[1] || !known_effect(a[2]) || a[5]>2) return OKL_INVALID;
        if(a[2]==1 && (a[3] || a[4] || a[5]!=1)) return OKL_INVALID;
        if((a[2]==0 || a[2]==8) && (a[3] || a[4] || a[5])) return OKL_INVALID;
        return all_zero(a+6+3*a[5],6-3*a[5])?OKL_OK:OKL_INVALID;
    case OKL_SET_COLOR_BRIGHTNESS:
        return !a[0] && !a[1]?OKL_OK:OKL_INVALID;
    case OKL_SET_WHITE_BRIGHTNESS:
        return !a[0] && a[1]==32 && known_effect(a[3]) &&
               (!a[3] || a[2]<=38)?OKL_OK:OKL_INVALID;
    case OKL_SET_TEMPERATURE:
        temperature=(uint16_t)((uint16_t)a[2]*256u+a[3]);
        return !a[0] && a[1]==32 && temperature>=3000 && temperature<=7000?OKL_OK:OKL_INVALID;
    case OKL_SET_FRAME:
        return all_zero(a,5) && a[8]==0?OKL_OK:OKL_INVALID;
    case OKL_CONFIRM_CONTROLLER:
        return !memcmp(a,"OKLC",4)?OKL_OK:OKL_INVALID;
    default: return OKL_OK;
    }
}

okl_result okl_request_build(okl_request *out, okl_command command,
                              const uint8_t *arguments, size_t size) {
    okl_request candidate;
    okl_result result;
    if(!out || size>OKL_ARGUMENT_BYTES || (size && !arguments)) return OKL_INVALID;
    memset(&candidate,0,sizeof(candidate));
    candidate.command=command; candidate.size=(uint8_t)size;
    if(size) memcpy(candidate.arguments,arguments,size);
    result=valid_request(&candidate);
    if(result==OKL_OK) *out=candidate;
    return result;
}

okl_result okl_request_get(okl_request *out, okl_command command) {
    uint8_t args[3]={0,0,0};
    if((unsigned)command>OKL_GET_TEMPERATURE && command!=OKL_GET_PART_ID && command!=OKL_GET_CONTROLLER_STATUS)
        return OKL_INVALID;
    if(command==OKL_GET_WHITE_BRIGHTNESS || command==OKL_GET_TEMPERATURE) args[1]=32;
    return okl_request_build(out,command,args,commands[command].size);
}

okl_result okl_request_static(okl_request *out, const uint8_t rgb[3]) {
    uint8_t args[12]={0,0,1,0,0,1,0,0,0,0,0,0};
    if(!rgb) return OKL_INVALID;
    memcpy(args+6,rgb,3);return okl_request_build(out,OKL_SET_EFFECT,args,sizeof(args));
}

okl_result okl_request_custom(okl_request *out) {
    const uint8_t args[12]={0,0,8,0,0,0,0,0,0,0,0,0};
    return okl_request_build(out,OKL_SET_EFFECT,args,sizeof(args));
}

okl_result okl_request_frame(okl_request *out, const uint8_t rgb[3]) {
    uint8_t args[9]={0,0,0,0,0,0,0,0,0};
    if(!rgb) return OKL_INVALID;
    memcpy(args+5,rgb,3);return okl_request_build(out,OKL_SET_FRAME,args,sizeof(args));
}

okl_result okl_request_color_brightness(okl_request *out, uint8_t brightness) {
    const uint8_t args[3]={0,0,brightness};
    return okl_request_build(out,OKL_SET_COLOR_BRIGHTNESS,args,sizeof(args));
}

okl_result okl_request_white_brightness(okl_request *out, uint8_t brightness, uint8_t effect) {
    const uint8_t args[4]={0,32,brightness,effect};
    return okl_request_build(out,OKL_SET_WHITE_BRIGHTNESS,args,sizeof(args));
}

okl_result okl_request_temperature(okl_request *out, uint16_t kelvin) {
    const uint8_t args[4]={0,32,(uint8_t)(kelvin>>8),(uint8_t)kelvin};
    return okl_request_build(out,OKL_SET_TEMPERATURE,args,sizeof(args));
}

okl_result okl_request_confirm_controller(okl_request *out) {
    return okl_request_build(out,OKL_CONFIRM_CONTROLLER,(const uint8_t *)"OKLC",4);
}

static uint8_t checksum(const uint8_t *report) {
    uint8_t value=0;
    size_t i;
    for(i=2;i<88;++i) value^=report[i];
    return value;
}

okl_result okl_report_encode(uint8_t out[OKL_REPORT_BYTES], uint8_t transaction,
                             uint8_t cls, uint8_t opcode,
                             const uint8_t *arguments, size_t size) {
    uint8_t report[OKL_REPORT_BYTES];
    if(!out || size>OKL_ARGUMENT_BYTES || (size && !arguments)) return OKL_INVALID;
    memset(report,0,sizeof(report));
    report[1]=transaction; report[5]=(uint8_t)size; report[6]=cls; report[7]=opcode;
    if(size) memcpy(report+8,arguments,size);
    report[88]=checksum(report);
    memcpy(out,report,sizeof(report));
    return OKL_OK;
}

okl_result okl_report_decode(okl_report *out, const uint8_t *bytes, size_t size) {
    okl_report report;
    if(!out || !bytes) return OKL_INVALID;
    if(size!=OKL_REPORT_BYTES || !all_zero(bytes+2,3) || bytes[5]>OKL_ARGUMENT_BYTES ||
       bytes[89] || bytes[88]!=checksum(bytes)) return OKL_PROTOCOL;
    memset(&report,0,sizeof(report));
    report.status=bytes[0]; report.transaction=bytes[1]; report.size=bytes[5];
    report.command_class=bytes[6]; report.opcode=bytes[7];
    memcpy(report.arguments,bytes+8,report.size);
    *out=report;
    return OKL_OK;
}

okl_result okl_nxp_init(okl_nxp *d, const okl_transport *t, const uint8_t identity[6]) {
    if(!d || !t || !identity || all_zero(identity,6) || (identity[0]&1) ||
       !t->now_us || !t->lock || !t->unlock || !t->arm_ready || !t->wait_ready || !t->transfer)
        return OKL_INVALID;
    memset(d,0,sizeof(*d)); d->transport=*t; memcpy(d->identity,identity,6);
    return OKL_OK;
}

static int is_getter_reply(const okl_reply *r, uint8_t opcode, uint8_t size) {
    return r && r->received && r->acknowledged && r->report.status==2 &&
           r->report.command_class==0 && r->report.opcode==opcode && r->report.size==size;
}

okl_result okl_reply_decode_firmware(okl_firmware_version *out, const okl_reply *reply) {
    if(!out || !reply) return OKL_INVALID;
    if(!is_getter_reply(reply,0x87,4)) return OKL_PROTOCOL;
    memcpy(out->component,reply->report.arguments,4);return OKL_OK;
}

okl_result okl_reply_decode_mode(uint8_t *out, const okl_reply *reply) {
    if(!out || !reply) return OKL_INVALID;
    if(!is_getter_reply(reply,0x84,1)) return OKL_PROTOCOL;
    *out=reply->report.arguments[0];return OKL_OK;
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

okl_result okl_reply_decode_part_id(uint32_t *out, const okl_reply *reply) {
    if(!out || !reply) return OKL_INVALID;
    if(!is_getter_reply(reply,0xfe,4)) return OKL_PROTOCOL;
    *out=read_u32(reply->report.arguments); return OKL_OK;
}

okl_result okl_reply_decode_controller_status(okl_controller_status *out, const okl_reply *reply) {
    okl_controller_status status={0}; const uint8_t *a;
    if(!out || !reply) return OKL_INVALID;
    if(!is_getter_reply(reply,0xfc,OKL_CONTROLLER_STATUS_BYTES)) return OKL_PROTOCOL;
    a=reply->report.arguments;
    if(memcmp(a,"OKLC",4) || a[4]!=OKL_CONTROLLER_ABI_MAJOR || a[5]!=OKL_CONTROLLER_ABI_MINOR ||
       a[6]>OKL_ROLE_LIGHTING || (a[7]&~3u)) return OKL_PROTOCOL;
    status.abi_major=a[4]; status.abi_minor=a[5]; status.role=a[6];
    status.trial_confirmed=a[7]&1u; status.boot_requested=(a[7]>>1)&1u;
    status.capabilities=read_u32(a+8);
    if(status.capabilities & ~(OKL_CAP_RECOVERY_READY|OKL_CAP_LIGHTING_READY)) return OKL_PROTOCOL;
    status.part_id=read_u32(a+12); status.uptime_ms=read_u32(a+16); status.reset_cause=read_u32(a+20);
    *out=status; return OKL_OK;
}

okl_result okl_reply_check_controller_confirmation(const okl_reply *reply) {
    if(!reply) return OKL_INVALID;
    return is_getter_reply(reply,0xfd,1) && reply->report.arguments[0]==1?OKL_OK:OKL_PROTOCOL;
}

uint64_t okl_nxp_default_deadline(const okl_nxp *d) {
    uint64_t now;
    if(!d || !d->transport.now_us) return 0;
    now=d->transport.now_us(d->transport.user);
    return now>UINT64_MAX-OKL_DEFAULT_TIMEOUT_US?UINT64_MAX:now+OKL_DEFAULT_TIMEOUT_US;
}

static okl_result deadline_check(okl_nxp *d, uint64_t deadline) {
    return d->transport.now_us(d->transport.user)>=deadline?OKL_TIMEOUT:OKL_OK;
}

static okl_result take_lock(okl_nxp *d, uint64_t deadline) {
    okl_result result;
    if(!d || !d->transport.now_us || !d->transport.lock) return OKL_INVALID;
    if(deadline_check(d,deadline)!=OKL_OK) return OKL_TIMEOUT;
    result=d->transport.lock(d->transport.user,deadline);
    if(result==OKL_OK && deadline_check(d,deadline)!=OKL_OK) {
        d->transport.unlock(d->transport.user); return OKL_TIMEOUT;
    }
    return result;
}

static okl_result exchange_locked(okl_nxp *d, const okl_request *request,
                                   okl_reply *reply, uint64_t deadline) {
    uint8_t tx[OKL_SPI_LIMIT], rx[OKL_SPI_LIMIT];
    size_t length;
    const descriptor *desc=&commands[request->command];
    okl_result result;
    okl_transport *t=&d->transport;
    memset(reply,0,sizeof(*reply));
    if(d->needs_recovery) return OKL_NEEDS_RECOVERY;
    if(deadline_check(d,deadline)!=OKL_OK) return OKL_TIMEOUT;
    result=t->arm_ready(t->user,deadline);
    if(result!=OKL_OK) {d->needs_recovery=1; return result;}
    if(deadline_check(d,deadline)!=OKL_OK) {d->needs_recovery=1;return OKL_TIMEOUT;}
    memset(tx,0,sizeof(tx));
    memcpy(tx,d->identity,6);
    ++d->transaction;
    (void)okl_report_encode(tx+7,d->transaction,desc->cls,desc->op,
                            request->arguments,request->size);
    reply->sent=1;
    result=t->transfer(t->user,tx,rx,97,deadline);
    if(result!=OKL_OK) goto failed;
    if((result=deadline_check(d,deadline))!=OKL_OK) goto failed;
    result=t->wait_ready(t->user,deadline);
    if(result!=OKL_OK) goto failed;
    if((result=deadline_check(d,deadline))!=OKL_OK) goto failed;
    memset(tx,0,sizeof(tx));
    result=t->transfer(t->user,tx,rx,2,deadline);
    if(result!=OKL_OK) goto failed;
    if((result=deadline_check(d,deadline))!=OKL_OK) goto failed;
    length=(size_t)rx[0]*256u+rx[1];
    /* This driver intentionally supports exactly one90-byte report. */
    if(length!=97) {result=OKL_PROTOCOL;goto failed;}
    result=t->transfer(t->user,tx,rx,length,deadline);
    if(result!=OKL_OK) goto failed;
    if((result=deadline_check(d,deadline))!=OKL_OK) goto failed;
    if((rx[6]==0 && memcmp(rx,d->identity,6)) ||
       (rx[6]==4 && !all_zero(rx,6)) || (rx[6]!=0 && rx[6]!=4)) {
        result=OKL_PROTOCOL;goto failed;
    }
    result=okl_report_decode(&reply->report,rx+7,90);
    if(result!=OKL_OK) goto failed;
    if(reply->report.transaction!=d->transaction || reply->report.command_class!=desc->cls ||
       reply->report.opcode!=desc->op) {result=OKL_PROTOCOL;goto failed;}
    reply->received=1;reply->bridge_kind=rx[6];
    if(reply->report.status!=2) return reply->report.status==8?OKL_OWNER_DENIED:OKL_REMOTE;
    reply->acknowledged=1;
    return OKL_OK;
failed:
    d->needs_recovery=1;
    return result;
}

okl_result okl_nxp_execute(okl_nxp *d, const okl_request *request,
                           okl_reply *reply, uint64_t deadline) {
    okl_result result;
    if(!reply) return OKL_INVALID;
    memset(reply,0,sizeof(*reply));
    if(valid_request(request)!=OKL_OK) return OKL_INVALID;
    result=take_lock(d,deadline);
    if(result!=OKL_OK) return result;
    result=exchange_locked(d,request,reply,deadline);
    d->transport.unlock(d->transport.user);
    return result;
}

okl_result okl_nxp_recover(okl_nxp *d, uint64_t deadline) {
    okl_result result=take_lock(d,deadline);
    if(result!=OKL_OK) return result;
    if(!d->transport.recover) result=OKL_NEEDS_RECOVERY;
    else result=d->transport.recover(d->transport.user,deadline);
    if(result==OKL_OK) result=deadline_check(d,deadline);
    d->needs_recovery=(uint8_t)(result!=OKL_OK);
    d->transport.unlock(d->transport.user);
    return result;
}

static okl_result perform(okl_nxp *d, okl_command command, const uint8_t *args,
                           size_t size, okl_reply *reply, uint64_t deadline) {
    okl_request request;
    okl_result result=okl_request_build(&request,command,args,size);
    return result==OKL_OK?exchange_locked(d,&request,reply,deadline):result;
}

static okl_result owner_locked(okl_nxp *d, okl_owner *owner, uint64_t deadline) {
    okl_reply reply;
    okl_owner value;
    okl_result result=perform(d,OKL_GET_OWNER,NULL,0,&reply,deadline);
    const uint8_t *a=reply.report.arguments;
    if(result!=OKL_OK) return result;
    /* Captured unclaimed replies omit the name-length byte entirely. */
    if(reply.report.size==7 && all_zero(a,7)) {
        memset(owner,0,sizeof(*owner));return OKL_OK;
    }
    if(reply.report.size<8 || a[0]>1 || a[7]>64 || reply.report.size<8u+a[7]) return OKL_PROTOCOL;
    memset(&value,0,sizeof(value));value.claimed=a[0];value.name_size=a[7];
    memcpy(value.identity,a+1,6);memcpy(value.name,a+8,value.name_size);*owner=value;
    return OKL_OK;
}

okl_result okl_nxp_get_owner(okl_nxp *d, okl_owner *owner, uint64_t deadline) {
    okl_result result;
    if(!owner) return OKL_INVALID;
    result=take_lock(d,deadline);
    if(result!=OKL_OK) return result;
    result=owner_locked(d,owner,deadline);d->transport.unlock(d->transport.user);return result;
}

okl_result okl_nxp_claim(okl_nxp *d, const uint8_t *name, size_t size, uint64_t deadline) {
    uint8_t args[72];okl_reply reply;okl_owner owner;okl_result result;
    if(size>64 || (size && !name)) return OKL_INVALID;
    result=take_lock(d,deadline);if(result!=OKL_OK) return result;
    memset(args,0,sizeof(args));args[0]=1;memcpy(args+1,d->identity,6);args[7]=(uint8_t)size;
    if(size) memcpy(args+8,name,size);
    result=perform(d,OKL_SET_OWNER,args,sizeof(args),&reply,deadline);
    if(result==OKL_OK) result=owner_locked(d,&owner,deadline);
    if(result==OKL_OK && (!owner.claimed || memcmp(owner.identity,d->identity,6))) result=OKL_VERIFY;
    d->transport.unlock(d->transport.user);return result;
}

okl_result okl_nxp_release(okl_nxp *d, uint64_t deadline) {
    uint8_t args[72];okl_reply reply;okl_owner owner;okl_result result=take_lock(d,deadline);
    if(result!=OKL_OK) return result;
    result=owner_locked(d,&owner,deadline);
    if(result==OKL_OK && owner.claimed) {
        if(memcmp(owner.identity,d->identity,6)) result=OKL_NOT_OWNER;
        else {
            memset(args,0,sizeof(args));memcpy(args+1,d->identity,6);
            result=perform(d,OKL_SET_OWNER,args,sizeof(args),&reply,deadline);
            if(result==OKL_OK) result=owner_locked(d,&owner,deadline);
            if(result==OKL_OK && owner.claimed) result=OKL_VERIFY;
        }
    }
    d->transport.unlock(d->transport.user);return result;
}

okl_result okl_nxp_read_state(okl_nxp *d, okl_light_state *state, uint64_t deadline) {
    static const uint8_t color[]={0,0},white[]={0,32,0};
    okl_light_state value;okl_reply reply;okl_result result;const uint8_t *a;
    if(!state) return OKL_INVALID;
    result=take_lock(d,deadline);if(result!=OKL_OK) return result;
    memset(&value,0,sizeof(value));
    result=perform(d,OKL_GET_EFFECT,color,2,&reply,deadline);if(result!=OKL_OK) goto done;
    a=reply.report.arguments;
    if(reply.report.size<6 || a[0] || a[1] || !known_effect(a[2]) || a[5]>2 ||
       reply.report.size!=6u+3u*a[5]) {result=OKL_PROTOCOL;goto done;}
    value.effect=a[2];value.flags=a[3];value.speed=a[4];value.color_count=a[5];memcpy(value.colors,a+6,3u*a[5]);
    result=perform(d,OKL_GET_WHITE_BRIGHTNESS,white,3,&reply,deadline);if(result!=OKL_OK) goto done;
    a=reply.report.arguments;
    if(reply.report.size!=4 || a[0] || a[1]!=32 || a[3]!=value.effect) {result=OKL_VERIFY;goto done;}
    value.white_brightness=a[2];
    result=perform(d,OKL_GET_COLOR_BRIGHTNESS,color,2,&reply,deadline);if(result!=OKL_OK) goto done;
    a=reply.report.arguments;
    if(reply.report.size!=3 || a[0] || a[1]) {result=OKL_PROTOCOL;goto done;}
    value.color_brightness=a[2];
    result=perform(d,OKL_GET_TEMPERATURE,white,2,&reply,deadline);if(result!=OKL_OK) goto done;
    a=reply.report.arguments;
    if(reply.report.size!=5 || a[0] || a[1]!=32 || a[4]) {result=OKL_PROTOCOL;goto done;}
    value.temperature_kelvin=(uint16_t)((uint16_t)a[2]*256u+a[3]);
    if(value.temperature_kelvin<3000 || value.temperature_kelvin>7000) {result=OKL_PROTOCOL;goto done;}
    *state=value;
done:
    d->transport.unlock(d->transport.user);return result;
}
