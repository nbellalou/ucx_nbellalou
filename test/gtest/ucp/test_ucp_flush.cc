/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ucp_test.h"

extern "C" {
#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_request.inl>
#include <uct/base/uct_iface.h>
}

static unsigned test_flush_call_count;

static ucs_status_t
test_flush_count_calls(uct_ep_h, unsigned, uct_completion_t *)
{
    ++test_flush_call_count;
    return UCS_ERR_IO_ERROR;
}

static uct_completion_t *test_flush_comp;
static uct_ep_h test_flush_eps[4];

static ucs_status_t
test_flush_inprogress(uct_ep_h ep, unsigned, uct_completion_t *comp)
{
    if (test_flush_call_count < ucs_static_array_size(test_flush_eps)) {
        test_flush_eps[test_flush_call_count] = ep;
    }

    ++test_flush_call_count;
    test_flush_comp = comp;
    return UCS_INPROGRESS;
}

static void test_flush_completion(ucp_request_t *req)
{
    ucp_request_complete_send(req, req->status);
}

class test_ucp_flush : public ucp_test {
public:
    static void get_test_variants(std::vector<ucp_test_variant> &variants)
    {
        add_variant(variants, UCP_FEATURE_TAG);
    }
};

UCS_TEST_P(test_ucp_flush, empty_lane_mask_skips_transport_flush)
{
    ucp_request_param_t param = {};
    ucp_ep_h ep;
    uct_iface_h iface;
    uct_ep_flush_func_t flush_func;
    ucs_status_ptr_t request;

    if (!is_self()) {
        UCS_TEST_SKIP_R("Direct flush interception requires self transport");
    }

    sender().connect(&receiver(), get_ep_params());
    ep         = sender().ep();
    iface      = ucp_ep_get_lane(ep, 0)->iface;
    flush_func = iface->ops.ep_flush;

    test_flush_call_count = 0;
    iface->ops.ep_flush   = test_flush_count_calls;

    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(ep->worker);
    request = ucp_ep_flush_lanes_internal(
            ep, 0, &param, NULL, test_flush_completion, "flush_lane_mask_test",
            UCT_FLUSH_FLAG_LOCAL, 0);
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(ep->worker);

    iface->ops.ep_flush = flush_func;

    EXPECT_EQ(0, test_flush_call_count);
    EXPECT_EQ(NULL, request);

    disconnect(sender());
    disconnect(receiver());
}

UCS_TEST_P(test_ucp_flush, replace_lane_during_selective_flush)
{
    ucp_request_param_t param = {};
    uct_ep_t replacement_lane = {};
    ucp_ep_h ep;
    uct_ep_h original_lane;
    uct_iface_h iface;
    uct_ep_flush_func_t flush_func;
    ucs_status_ptr_t request;

    if (!is_self()) {
        UCS_TEST_SKIP_R("Direct flush interception requires self transport");
    }

    sender().connect(&receiver(), get_ep_params());
    ep                       = sender().ep();
    original_lane            = ucp_ep_get_lane(ep, 0);
    iface                    = original_lane->iface;
    replacement_lane.iface   = iface;
    flush_func               = iface->ops.ep_flush;
    test_flush_call_count    = 0;
    test_flush_comp          = NULL;
    test_flush_eps[0]        = NULL;
    test_flush_eps[1]        = NULL;
    iface->ops.ep_flush      = test_flush_inprogress;

    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(ep->worker);
    request = ucp_ep_flush_lanes_internal(
            ep, 0, &param, NULL, test_flush_completion,
            "replace_lane_during_flush", UCT_FLUSH_FLAG_LOCAL, UCS_BIT(0));
    EXPECT_TRUE(UCS_PTR_IS_PTR(request));
    EXPECT_EQ(1u, test_flush_call_count);
    EXPECT_EQ(original_lane, test_flush_eps[0]);

    ucp_ep_set_lane(ep, 0, &replacement_lane);
    EXPECT_TRUE(test_flush_comp != NULL);
    if (test_flush_comp != NULL) {
        uct_invoke_completion(test_flush_comp, UCS_OK);
    }

    EXPECT_EQ(2u, test_flush_call_count);
    EXPECT_EQ(&replacement_lane, test_flush_eps[1]);

    EXPECT_TRUE(test_flush_comp != NULL);
    if (test_flush_comp != NULL) {
        uct_invoke_completion(test_flush_comp, UCS_OK);
    }
    ucp_ep_set_lane(ep, 0, original_lane);
    iface->ops.ep_flush = flush_func;
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(ep->worker);

    EXPECT_UCS_OK(ucp_request_check_status(request));
    ucp_request_release(request);

    disconnect(sender());
    disconnect(receiver());
}

UCS_TEST_P(test_ucp_flush, replace_other_lane_during_inprogress_flush)
{
    ucp_request_param_t param = {};
    uct_ep_t original_second_lane = {};
    uct_ep_t replacement_second_lane = {};
    ucp_ep_h ep;
    ucp_ep_config_t *ep_config;
    uct_ep_h original_lane;
    uct_iface_h iface;
    uct_ep_flush_func_t flush_func;
    ucs_status_ptr_t request;
    ucp_lane_index_t saved_num_lanes;

    if (!is_self()) {
        UCS_TEST_SKIP_R("Direct flush interception requires self transport");
    }

    sender().connect(&receiver(), get_ep_params());
    ep              = sender().ep();
    ep_config       = const_cast<ucp_ep_config_t*>(ucp_ep_config(ep));
    saved_num_lanes = ep_config->key.num_lanes;
    ASSERT_EQ(1u, saved_num_lanes);

    original_lane                 = ucp_ep_get_lane(ep, 0);
    iface                         = original_lane->iface;
    original_second_lane.iface    = iface;
    replacement_second_lane.iface = iface;
    flush_func                    = iface->ops.ep_flush;
    test_flush_call_count         = 0;
    test_flush_comp               = NULL;
    test_flush_eps[0]             = NULL;
    test_flush_eps[1]             = NULL;
    test_flush_eps[2]             = NULL;
    test_flush_eps[3]             = NULL;
    ep_config->key.num_lanes      = 2;
    ucp_ep_set_lane(ep, 1, &original_second_lane);
    iface->ops.ep_flush           = test_flush_inprogress;

    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(ep->worker);
    request = ucp_ep_flush_lanes_internal(
            ep, 0, &param, NULL, test_flush_completion,
            "replace_other_lane_during_flush", UCT_FLUSH_FLAG_LOCAL,
            UCS_BIT(0) | UCS_BIT(1));
    EXPECT_TRUE(UCS_PTR_IS_PTR(request));
    EXPECT_EQ(2u, test_flush_call_count);
    EXPECT_EQ(original_lane, test_flush_eps[0]);
    EXPECT_EQ(&original_second_lane, test_flush_eps[1]);

    ucp_ep_set_lane(ep, 1, &replacement_second_lane);
    EXPECT_TRUE(test_flush_comp != NULL);
    if (test_flush_comp != NULL) {
        uct_invoke_completion(test_flush_comp, UCS_OK);
        uct_invoke_completion(test_flush_comp, UCS_OK);
    }

    EXPECT_EQ(4u, test_flush_call_count);
    EXPECT_EQ(original_lane, test_flush_eps[2]);
    EXPECT_EQ(&replacement_second_lane, test_flush_eps[3]);

    EXPECT_TRUE(test_flush_comp != NULL);
    if (test_flush_comp != NULL) {
        uct_invoke_completion(test_flush_comp, UCS_OK);
        uct_invoke_completion(test_flush_comp, UCS_OK);
    }
    ucp_ep_set_lane(ep, 1, NULL);
    ep_config->key.num_lanes = saved_num_lanes;
    iface->ops.ep_flush      = flush_func;
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(ep->worker);

    EXPECT_UCS_OK(ucp_request_check_status(request));
    ucp_request_release(request);

    disconnect(sender());
    disconnect(receiver());
}

UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_flush, self, "self")
