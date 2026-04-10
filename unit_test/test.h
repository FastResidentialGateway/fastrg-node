#ifndef TEST_H
#define TEST_H

#include "../src/fastrg.h"

void test_dhcp_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_ppp_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_ppp_fsm(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_avl_tree(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_utils(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_nat(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_etcd_integration(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_dp_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_dbg(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_config(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_northbound(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_dns_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_dns_cache(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);
void test_dnsd(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass);

#endif // TEST_H