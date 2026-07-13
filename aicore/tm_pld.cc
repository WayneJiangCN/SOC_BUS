#include "tm_pld.h"

using namespace std;

INIT_TM_PLD_GID;  
INIT_TM_PLD_TYPEID;

//-------------------------------------------------------------------------------------------------
TmPld::TmPld()
: gid(++cur_gid)
, content(0)
{
}


TmPld::TmPld(any_t content) 
: gid(++cur_gid)
, content(content)
{
}

TmPld::TmPld(uint32_t type_id, any_t content)
: gid(++cur_gid)
, type_id(type_id)
, content(content)
{
}

TmPld::TmPld(pld_cmd_t cmd, uint64_t addr, uint32_t size)
: gid(++cur_gid)
, cmd(cmd)
, addr(addr)
, slv_addr(addr)
, size(size)
, content(0)
{
}

TmPld::TmPld(pld_cmd_t cmd, uint64_t addr, uint32_t size, pld_data_t data)
: gid(++cur_gid)
, cmd(cmd)
, addr(addr)
, slv_addr(addr)
, size(size)
, data(data)
, content(0)
{
}


TmPld::TmPld(pld_cmd_t cmd, uint64_t mst_addr, uint64_t slv_addr, uint32_t size)
: gid(++cur_gid)
, cmd(cmd)
, addr(slv_addr)
, mst_addr(mst_addr)
, slv_addr(slv_addr)
, size(size)
, content(0)

{
}

TmPld::TmPld(std::shared_ptr<TmPld> pld)
: gid(pld->gid)
, type_id(pld->type_id)
, content(pld->content)
, cmd(pld->cmd)
, addr(pld->addr)
, mst_id(pld->mst_id)
, slv_id(pld->slv_id)
, mst_addr(pld->mst_addr)
, slv_addr(pld->slv_addr)
, size(pld->size)
, rsp(pld->rsp)
, buf_u8(pld->buf_u8)
, buf_u32(pld->buf_u32)
, buf_u64(pld->buf_u64)
, data(pld->data)
, chan(pld->chan)
, latency(pld->latency)
, rsp_count(pld->rsp_count)
, ts(pld->ts)
, tnx_id(pld->tnx_id)
, tag_id(pld->tag_id)
, smmu_tnx_id(pld->smmu_tnx_id)
, ring_in_dir(pld->ring_in_dir)
, ring_out_dir(pld->ring_out_dir)
, ring_traffic_class(pld->ring_traffic_class)
, ring_rsp_lane(pld->ring_rsp_lane)
{

}

TmPld::~TmPld()
{
}


//-------------------------------------------------------------------------------------------------
void TmPld::set_ts(uint64_t ts) {
    this->ts = ts;
}

uint64_t TmPld::get_ts() {
    return this->ts;
}

uint32_t TmPld::reg_type_id() {
    return ++cur_type_id; //lint !e53
}

bool TmPld::is_type(uint32_t type_id) {
    return this->type_id == type_id;
}


//-------------------------------------------------------------------------------------------------
void TmPld::print() {
    cout << dec << noshowbase;
    cout << "[TmPld]: gid: " << gid;
    cout << ", cmd: " << (uint32_t)cmd;
    cout << hex << showbase;
    cout << ", mst_addr: " << mst_addr;
    cout << ", slv_addr: " << slv_addr;
    cout << ", size: " << size << endl;
    cout << dec << noshowbase;
}

void TmPld::print(std::string prefix) {
    cout << prefix;
    this->print();
}


//-------------------------------------------------------------------------------------------------
// End
