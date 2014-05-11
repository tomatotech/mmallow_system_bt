/******************************************************************************
 *
 *  Copyright (C) 2014 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "bt_target.h"

#include "btm_ble_api.h"
#include "bt_types.h"
#include "bt_utils.h"
#include "btu.h"
#include "btm_int.h"
#include "hcimsgs.h"

#if (BLE_INCLUDED == TRUE && BLE_BATCH_SCAN_INCLUDED == TRUE)

tBTM_BLE_BATCH_SCAN_CB ble_batchscan_cb;


/* length of each batch scan command */
#define BTM_BLE_BATCH_SCAN_STORAGE_CFG_LEN      4
#define BTM_BLE_BATCH_SCAN_PARAM_CONFIG_LEN    12
#define BTM_BLE_BATCH_SCAN_ENB_DISB_LEN         2
#define BTM_BLE_BATCH_SCAN_READ_RESULTS_LEN     2

#define BTM_BLE_BATCH_SCAN_CB_EVT_MASK       0xF0
#define BTM_BLE_BATCH_SCAN_SUBCODE_MASK      0x0F

/*******************************************************************************
**  Local functions
*******************************************************************************/

/*******************************************************************************
**
** Function         btm_ble_batchscan_filter_track_adv_vse_cback
**
** Description      VSE callback for batch scan, filter, and tracking events.
**
** Returns          None
**
*******************************************************************************/
void btm_ble_batchscan_filter_track_adv_vse_cback(UINT8 len, UINT8 *p)
{
    UINT8   sub_event;
    UINT8   reason;

    STREAM_TO_UINT8(sub_event, p);

    BTM_TRACE_EVENT("btm_ble_batchscan_filter_track_adv_vse_cback called with event:%x", sub_event);
    if (HCI_VSE_SUBCODE_BLE_THRESHOLD_SUB_EVT == sub_event &&
        NULL != ble_batchscan_cb.p_thres_cback)
    {
        ble_batchscan_cb.p_thres_cback(ble_batchscan_cb.ref_value);
    }
}

/*******************************************************************************
**
** Function         btm_ble_batchscan_enq_op_q
**
** Description      enqueue a batchscan operation in q to check command complete
**                  status
**
** Returns          void
**
*******************************************************************************/
void btm_ble_batchscan_enq_op_q(UINT8 opcode, tBTM_BLE_BATCH_SCAN_STATE cur_state,
                                          UINT8 cb_evt, tBTM_BLE_REF_VALUE ref_value)
{
    ble_batchscan_cb.op_q.sub_code[ble_batchscan_cb.op_q.next_idx] = (opcode |(cb_evt << 4));
    ble_batchscan_cb.op_q.cur_state[ble_batchscan_cb.op_q.next_idx] = cur_state;
    ble_batchscan_cb.op_q.ref_value[ble_batchscan_cb.op_q.next_idx] = ref_value;
    BTM_TRACE_DEBUG("btm_ble_batchscan_enq_op_q: subcode:%d, Cur_state:%d, ref_value:%d",
        ble_batchscan_cb.op_q.sub_code[ble_batchscan_cb.op_q.next_idx],
        ble_batchscan_cb.op_q.cur_state[ble_batchscan_cb.op_q.next_idx],
        ble_batchscan_cb.op_q.ref_value[ble_batchscan_cb.op_q.next_idx]);
    ble_batchscan_cb.op_q.next_idx = (ble_batchscan_cb.op_q.next_idx + 1)
                                        % BTM_BLE_BATCH_SCAN_MAX;
}

/*******************************************************************************
**
** Function         btm_ble_batchscan_deq_op_q
**
** Description      dequeue a batch scan operation from q when command complete
**                  is received
**
** Returns          void
**
*******************************************************************************/
void btm_ble_batchscan_deq_op_q(UINT8 *p_opcode,tBTM_BLE_BATCH_SCAN_STATE *cur_state,
                                          UINT8 *p_cb_evt, tBTM_BLE_REF_VALUE *p_ref)
{
    *p_cb_evt = (ble_batchscan_cb.op_q.sub_code[ble_batchscan_cb.op_q.pending_idx] >> 4);
    *p_opcode = (ble_batchscan_cb.op_q.sub_code[ble_batchscan_cb.op_q.pending_idx]
                                            & BTM_BLE_BATCH_SCAN_SUBCODE_MASK);
    *p_ref = ble_batchscan_cb.op_q.ref_value[ble_batchscan_cb.op_q.pending_idx];
    *cur_state = (ble_batchscan_cb.op_q.cur_state[ble_batchscan_cb.op_q.pending_idx]);
    ble_batchscan_cb.op_q.pending_idx = (ble_batchscan_cb.op_q.pending_idx + 1)
                                            % BTM_BLE_BATCH_SCAN_MAX;
}

/*******************************************************************************
**
** Function         btm_ble_batchscan_vsc_cmpl_cback
**
** Description      Batch scan VSC complete callback
**
** Parameters       p_params - VSC completed callback parameters
**
** Returns          void
**
*******************************************************************************/
void btm_ble_batchscan_vsc_cmpl_cback (tBTM_VSC_CMPL *p_params)
{
    UINT8  *p = p_params->p_param_buf;
    UINT16  len = p_params->param_len;
    tBTM_BLE_REF_VALUE ref_value = 0;

    UINT8  status = 0, subcode = 0, opcode = 0;
    UINT8 report_format = 0, num_records = 0, cb_evt = 0;
    tBTM_BLE_BATCH_SCAN_STATE cur_state = 0;

    if (len < 2)
    {
        BTM_TRACE_ERROR("wrong length for btm_ble_batch_scan_vsc_cmpl_cback");
        btm_ble_batchscan_deq_op_q(&opcode, &cur_state, &cb_evt, &ref_value);
        return;
    }

    STREAM_TO_UINT8(status, p);
    STREAM_TO_UINT8(subcode, p);

    btm_ble_batchscan_deq_op_q(&opcode, &cur_state, &cb_evt, &ref_value);

    BTM_TRACE_DEBUG("btm_ble_batchscan op_code = %02x state = %02x cb_evt = %02x,ref_value=%d",
        opcode, cur_state, cb_evt, ref_value);

    if (opcode != subcode)
    {
        BTM_TRACE_ERROR("Got unexpected VSC cmpl, expected: %d got: %d",subcode,opcode);
        return;
    }

    switch (subcode)
    {
        case BTM_BLE_BATCH_SCAN_ENB_DISAB_CUST_FEATURE:
        {
                if(BTM_SUCCESS == status && BTM_BLE_SCAN_ENABLE_CALLED == cur_state)
                    ble_batchscan_cb.cur_state = BTM_BLE_SCAN_ENABLED_STATE;
                else
                if(BTM_BLE_SCAN_ENABLE_CALLED == cur_state)
             {
                BTM_TRACE_ERROR("SCAN_ENB_DISAB_CUST_FEATURE - Invalid state after enb");
                    ble_batchscan_cb.cur_state = BTM_BLE_SCAN_INVALID_STATE;
             }

                if(BTM_SUCCESS == status && BTM_BLE_SCAN_DISABLE_CALLED == cur_state)
                    ble_batchscan_cb.cur_state = BTM_BLE_SCAN_DISABLED_STATE;
                else
                if(BTM_BLE_SCAN_DISABLE_CALLED == cur_state)
             {
                 BTM_TRACE_ERROR("SCAN_ENB_DISAB_CUST_FEATURE - Invalid state after disabled");
                    ble_batchscan_cb.cur_state = BTM_BLE_SCAN_INVALID_STATE;
             }
             BTM_TRACE_DEBUG("BTM_BLE_BATCH_SCAN_ENB_DISAB_CUST_FEAT status = %d, state: %d,evt=%d",
                status, ble_batchscan_cb.cur_state, cb_evt);

            if(cb_evt != 0 && NULL != ble_batchscan_cb.p_setup_cback)
                ble_batchscan_cb.p_setup_cback(cb_evt, ref_value, status);
            break;
        }

        case BTM_BLE_BATCH_SCAN_SET_STORAGE_PARAM:
        {
            BTM_TRACE_DEBUG("BTM_BLE_BATCH_SCAN_SET_STORAGE_PARAM status = %d, evt=%d",
                            status, cb_evt);
            if(cb_evt != 0 && NULL != ble_batchscan_cb.p_setup_cback)
              ble_batchscan_cb.p_setup_cback(cb_evt, ref_value, status);
            break;
        }

        case BTM_BLE_BATCH_SCAN_SET_PARAMS:
        {
            BTM_TRACE_DEBUG("BTM_BLE_BATCH_SCAN_SET_PARAMS status = %d,evt=%d", status, cb_evt);
            if(cb_evt != 0 && NULL != ble_batchscan_cb.p_setup_cback)
              ble_batchscan_cb.p_setup_cback(cb_evt, ref_value, status);
            break;
        }

        case BTM_BLE_BATCH_SCAN_READ_RESULTS:
        {
            if(cb_evt != 0 && NULL != ble_batchscan_cb.p_scan_rep_cback)
            {
                STREAM_TO_UINT8(report_format,p);
                STREAM_TO_UINT8(num_records, p);
                p = (uint8_t *)(p_params->p_param_buf + 4);
                BTM_TRACE_DEBUG("BTM_BLE_BATCH_SCAN_READ_RESULTS status=%d,len=%d,rec=%d",
                    status, len-4, num_records);
                ble_batchscan_cb.p_scan_rep_cback(ref_value,report_format,
                                                     num_records,(len-4),p,status);
            }
            break;
        }

        default:
            break;
    }

    return;
}

/*******************************************************************************
**
** Function         btm_ble_set_storage_config
**
** Description      This function writes the storage configuration in controller
**
** Parameters       batch_scan_full_max -Max storage space (in %) allocated to full scanning
**                  batch_scan_trunc_max -Max storage space (in %) allocated to truncated scanning
**                  batch_scan_notify_threshold - Setup notification level based on total space
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_set_storage_config(UINT8 batch_scan_full_max, UINT8 batch_scan_trunc_max,
                                       UINT8 batch_scan_notify_threshold)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    UINT8 param[BTM_BLE_BATCH_SCAN_STORAGE_CFG_LEN], *pp;

    pp = param;
    memset(param, 0, BTM_BLE_BATCH_SCAN_STORAGE_CFG_LEN);

    UINT8_TO_STREAM (pp, BTM_BLE_BATCH_SCAN_SET_STORAGE_PARAM);
    UINT8_TO_STREAM (pp, batch_scan_full_max);
    UINT8_TO_STREAM (pp, batch_scan_trunc_max);
    UINT8_TO_STREAM (pp, batch_scan_notify_threshold);

    if ((status = BTM_VendorSpecificCommand (HCI_BLE_BATCH_SCAN_OCF,
                BTM_BLE_BATCH_SCAN_STORAGE_CFG_LEN, param,
                btm_ble_batchscan_vsc_cmpl_cback))!= BTM_CMD_STARTED)
    {
        BTM_TRACE_ERROR("btm_ble_set_storage_config %d", status);
        return BTM_ILLEGAL_VALUE;
    }

    return status;
}

/*******************************************************************************
**
** Function         btm_ble_set_batchscan_param
**
** Description      This function writes the batch scan params in controller
**
** Parameters       scan_mode -Batch scan mode
**                  scan_interval - Scan interval
**                  scan_window  - Scan window
**                  discard_rule -Discard rules
**                  addr_type - Address type
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_set_batchscan_param(tBTM_BLE_BATCH_SCAN_MODE scan_mode,
                     UINT32 scan_interval, UINT32 scan_window, tBLE_ADDR_TYPE addr_type,
                     tBTM_BLE_DISCARD_RULE discard_rule)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    UINT8 scan_param[BTM_BLE_BATCH_SCAN_PARAM_CONFIG_LEN], *pp_scan;

    pp_scan = scan_param;
    memset(scan_param, 0, BTM_BLE_BATCH_SCAN_PARAM_CONFIG_LEN);

    UINT8_TO_STREAM (pp_scan, BTM_BLE_BATCH_SCAN_SET_PARAMS);
    UINT8_TO_STREAM (pp_scan, scan_mode);
    UINT32_TO_STREAM (pp_scan, scan_interval);
    UINT32_TO_STREAM (pp_scan, scan_window);
    UINT8_TO_STREAM (pp_scan, addr_type);
    UINT8_TO_STREAM (pp_scan, discard_rule);

    if ((status = BTM_VendorSpecificCommand (HCI_BLE_BATCH_SCAN_OCF,
            BTM_BLE_BATCH_SCAN_PARAM_CONFIG_LEN,
            scan_param, btm_ble_batchscan_vsc_cmpl_cback))!= BTM_CMD_STARTED)
    {
        BTM_TRACE_ERROR("btm_ble_set_batchscan_param %d", status);
        return BTM_ILLEGAL_VALUE;
    }

    return status;
}

/*******************************************************************************
**
** Function         btm_ble_enable_disable_batchscan
**
** Description      This function enables the customer specific feature in controller
**
** Parameters       enable_disable: true - enable, false - disable
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_enable_disable_batchscan(BOOLEAN enable_disable)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    UINT8 enb_disble = 0x01;
    UINT8 enable_param[BTM_BLE_BATCH_SCAN_ENB_DISB_LEN], *pp_enable;

    if(!enable_disable)
        enb_disble = 0x00;

    pp_enable = enable_param;
    memset(enable_param, 0, BTM_BLE_BATCH_SCAN_ENB_DISB_LEN);

    UINT8_TO_STREAM (pp_enable, BTM_BLE_BATCH_SCAN_ENB_DISAB_CUST_FEATURE);
    UINT8_TO_STREAM (pp_enable, enb_disble);

    if ((status = BTM_VendorSpecificCommand (HCI_BLE_BATCH_SCAN_OCF,
             BTM_BLE_BATCH_SCAN_ENB_DISB_LEN, enable_param,
             btm_ble_batchscan_vsc_cmpl_cback))!= BTM_CMD_STARTED)
    {
        status = BTM_MODE_UNSUPPORTED;
        BTM_TRACE_ERROR("btm_ble_enable_disable_batchscan %d", status);
        return BTM_ILLEGAL_VALUE;
    }

    if(enable_disable)
        ble_batchscan_cb.cur_state = BTM_BLE_SCAN_ENABLE_CALLED;
    else
        ble_batchscan_cb.cur_state = BTM_BLE_SCAN_DISABLE_CALLED;
    return status;
}

/*******************************************************************************
**
** Function         btm_ble_read_batchscan_reports
**
** Description      This function reads the reports from controller
**
** Parameters       scan_mode - The mode for which the reports are to be read out from the controller
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_read_batchscan_reports(tBTM_BLE_BATCH_SCAN_MODE scan_mode)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    UINT8 param[BTM_BLE_BATCH_SCAN_READ_RESULTS_LEN], *pp;
    pp = param;

    memset(param, 0, BTM_BLE_BATCH_SCAN_READ_RESULTS_LEN);

    UINT8_TO_STREAM (pp, BTM_BLE_BATCH_SCAN_READ_RESULTS);
    UINT8_TO_STREAM (pp, scan_mode);

    if ((status = BTM_VendorSpecificCommand (HCI_BLE_BATCH_SCAN_OCF,
            BTM_BLE_BATCH_SCAN_READ_RESULTS_LEN, param, btm_ble_batchscan_vsc_cmpl_cback))
            != BTM_CMD_STARTED)
    {
        BTM_TRACE_ERROR("btm_ble_read_batchscan_reports %d", status);
        return BTM_ILLEGAL_VALUE;
    }

    return status;
}

/*******************************************************************************
**
** Function         BTM_BleSetStorageConfig
**
** Description      This function is called to write storage config params.
**
** Parameters:      batch_scan_full_max - Max storage space (in %) allocated to full style
**                  batch_scan_trunc_max - Max storage space (in %) allocated to trunc style
**                  batch_scan_notify_threshold - Setup notification level based on total space
**                  p_setup_cback - Setup callback pointer
**                  p_thres_cback - Threshold callback pointer
**                  p_rep_cback - Reports callback pointer
**                  ref_value - Reference value
**
** Returns          tBTM_STATUS
**
*******************************************************************************/
tBTM_STATUS BTM_BleSetStorageConfig(UINT8 batch_scan_full_max, UINT8 batch_scan_trunc_max,
                                        UINT8 batch_scan_notify_threshold,
                                        tBTM_BLE_SCAN_SETUP_CBACK *p_setup_cback,
                                        tBTM_BLE_SCAN_THRESHOLD_CBACK *p_thres_cback,
                                        tBTM_BLE_SCAN_REP_CBACK* p_rep_cback,
                                        tBTM_BLE_REF_VALUE ref_value)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    tBTM_BLE_VSC_CB cmn_ble_vsc_cb;

    BTM_TRACE_EVENT (" BTM_BleSetStorageConfig: %d", ble_batchscan_cb.cur_state);

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    BTM_BleGetVendorCapabilities(&cmn_ble_vsc_cb);

    if (0 == cmn_ble_vsc_cb.tot_scan_results_strg)
    {
        BTM_TRACE_ERROR("Controller does not support batch scan");
        return BTM_ERR_PROCESSING;
    }

    ble_batchscan_cb.p_setup_cback = p_setup_cback;
    ble_batchscan_cb.p_thres_cback = p_thres_cback;
    ble_batchscan_cb.p_scan_rep_cback = p_rep_cback;
    ble_batchscan_cb.ref_value = ref_value;

    if (batch_scan_full_max > BTM_BLE_ADV_SCAN_FULL_MAX ||
        batch_scan_trunc_max > BTM_BLE_ADV_SCAN_TRUNC_MAX ||
        batch_scan_notify_threshold > BTM_BLE_ADV_SCAN_THR_MAX)
    {
        BTM_TRACE_ERROR("Illegal set storage config params");
        return BTM_ILLEGAL_VALUE;
    }

     if (BTM_BLE_SCAN_INVALID_STATE == ble_batchscan_cb.cur_state
        || BTM_BLE_SCAN_DISABLED_STATE == ble_batchscan_cb.cur_state ||
         BTM_BLE_SCAN_DISABLE_CALLED == ble_batchscan_cb.cur_state)
    {
         status = btm_ble_enable_disable_batchscan(TRUE);
        if(BTM_CMD_STARTED != status)
            return status;
         ble_batchscan_cb.cur_state = BTM_BLE_SCAN_ENABLE_CALLED;
         btm_ble_batchscan_enq_op_q(BTM_BLE_BATCH_SCAN_ENB_DISAB_CUST_FEATURE,
                                    BTM_BLE_SCAN_ENABLE_CALLED, 0, ref_value);
    }

    status = btm_ble_set_storage_config(batch_scan_full_max, batch_scan_trunc_max,
                                        batch_scan_notify_threshold);
    if(BTM_CMD_STARTED != status)
       return status;
            /* The user needs to be provided scan config storage event */
     btm_ble_batchscan_enq_op_q(BTM_BLE_BATCH_SCAN_SET_STORAGE_PARAM, ble_batchscan_cb.cur_state,
                                   BTM_BLE_BATCH_SCAN_CFG_STRG_EVT, ref_value);

    return status;
}


/*******************************************************************************
**
** Function         BTM_BleEnableBatchScan
**
** Description      This function is called to configure and enable batch scanning
**
** Parameters:      scan_mode -Batch scan mode
**                  scan_interval - Scan interval value
**                  scan_window - Scan window value
**                  discard_rule - Data discard rule
**                  ref_value - Reference value
**
** Returns          tBTM_STATUS
**
*******************************************************************************/
tBTM_STATUS BTM_BleEnableBatchScan(tBTM_BLE_BATCH_SCAN_MODE scan_mode,
            UINT32 scan_interval, UINT32 scan_window, tBLE_ADDR_TYPE addr_type,
            tBTM_BLE_DISCARD_RULE discard_rule, tBTM_BLE_REF_VALUE ref_value)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    tBTM_BLE_VSC_CB cmn_ble_vsc_cb;
    BTM_TRACE_EVENT (" BTM_BleEnableBatchScan");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    BTM_BleGetVendorCapabilities(&cmn_ble_vsc_cb);

    if (0 == cmn_ble_vsc_cb.tot_scan_results_strg)
    {
        BTM_TRACE_ERROR("Controller does not support batch scan");
        return BTM_ERR_PROCESSING;
    }

    BTM_TRACE_DEBUG("BTM_BleEnableBatchScan: %d, %x, %x, %d, %d", scan_mode, scan_interval,
                                        scan_window, discard_rule, ble_batchscan_cb.cur_state);

    /* Only 16 bits will be used for scan interval and scan window as per agreement with Google */
    /* So the standard LE range would suffice for scan interval and scan window */
    if ((BTM_BLE_VALID_PRAM(scan_interval, BTM_BLE_SCAN_INT_MIN, BTM_BLE_SCAN_INT_MAX) ||
        BTM_BLE_VALID_PRAM(scan_window, BTM_BLE_SCAN_WIN_MIN, BTM_BLE_SCAN_WIN_MAX))
        && (BTM_BLE_BATCH_SCAN_MODE_PASS == scan_mode || BTM_BLE_BATCH_SCAN_MODE_ACTI == scan_mode
        || BTM_BLE_BATCH_SCAN_MODE_PASS_ACTI == scan_mode)
        && (BTM_BLE_DISCARD_OLD_ITEMS == discard_rule ||
        BTM_BLE_DISCARD_LOWER_RSSI_ITEMS == discard_rule))
    {
        if (BTM_BLE_SCAN_INVALID_STATE == ble_batchscan_cb.cur_state
            || BTM_BLE_SCAN_DISABLED_STATE == ble_batchscan_cb.cur_state ||
            BTM_BLE_SCAN_DISABLE_CALLED == ble_batchscan_cb.cur_state)
        {
        status = btm_ble_enable_disable_batchscan(TRUE);
        if(BTM_CMD_STARTED != status)
           return status;
        btm_ble_batchscan_enq_op_q(BTM_BLE_BATCH_SCAN_ENB_DISAB_CUST_FEATURE,
                                   BTM_BLE_SCAN_ENABLE_CALLED, 0, ref_value);
        }

        ble_batchscan_cb.scan_mode = scan_mode;
        /* This command starts batch scanning, if enabled */
        status = btm_ble_set_batchscan_param(scan_mode, scan_interval, scan_window, addr_type,
                    discard_rule);
        if(BTM_CMD_STARTED != status)
            return status;

        /* The user needs to be provided scan enable event */
        btm_ble_batchscan_enq_op_q(BTM_BLE_BATCH_SCAN_SET_PARAMS, ble_batchscan_cb.cur_state,
                                   BTM_BLE_BATCH_SCAN_ENABLE_EVT, ref_value);
    }
    else
    {
        BTM_TRACE_ERROR("Illegal enable scan params");
        return BTM_ILLEGAL_VALUE;
    }
    return status;
}

/*******************************************************************************
**
** Function         BTM_BleDisableBatchScan
**
** Description      This function is called to disable batch scanning
**
** Parameters:      ref_value - Reference value
**
** Returns          tBTM_STATUS
**
*******************************************************************************/
tBTM_STATUS BTM_BleDisableBatchScan(tBTM_BLE_REF_VALUE ref_value)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    tBTM_BLE_VSC_CB cmn_ble_vsc_cb;
    BTM_TRACE_EVENT (" BTM_BleDisableBatchScan");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    BTM_BleGetVendorCapabilities(&cmn_ble_vsc_cb);

    if (0 == cmn_ble_vsc_cb.tot_scan_results_strg)
    {
        BTM_TRACE_ERROR("Controller does not support batch scan");
        return BTM_ERR_PROCESSING;
    }

    status = btm_ble_enable_disable_batchscan(FALSE);
    if(BTM_CMD_STARTED == status)
    {
       /* The user needs to be provided scan disable event */
       btm_ble_batchscan_enq_op_q(BTM_BLE_BATCH_SCAN_ENB_DISAB_CUST_FEATURE,
                                  BTM_BLE_SCAN_DISABLE_CALLED, BTM_BLE_BATCH_SCAN_DISABLE_EVT,
                                  ref_value);
    }

    return status;
}

/*******************************************************************************
**
** Function         BTM_BleReadScanReports
**
** Description      This function is called to start reading batch scan reports
**
** Parameters:      scan_mode - Batch scan mode
**                  ref_value - Reference value
**
** Returns          tBTM_STATUS
**
*******************************************************************************/
tBTM_STATUS BTM_BleReadScanReports(tBTM_BLE_BATCH_SCAN_MODE scan_mode,
                                             tBTM_BLE_REF_VALUE ref_value)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    tBTM_BLE_VSC_CB cmn_ble_vsc_cb;
    UINT8 read_scan_mode = 0;

    BTM_TRACE_EVENT (" BTM_BleReadScanReports");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    BTM_BleGetVendorCapabilities(&cmn_ble_vsc_cb);

    if (0 == cmn_ble_vsc_cb.tot_scan_results_strg)
    {
        BTM_TRACE_ERROR("Controller does not support batch scan");
        return BTM_ERR_PROCESSING;
    }

    /*  Check if the requested scan mode has already been setup by the user */
    read_scan_mode = ble_batchscan_cb.scan_mode & BTM_BLE_BATCH_SCAN_MODE_ACTI;
    if(0 == read_scan_mode)
       read_scan_mode = ble_batchscan_cb.scan_mode & BTM_BLE_BATCH_SCAN_MODE_PASS;

    if(read_scan_mode > 0 && (BTM_BLE_BATCH_SCAN_MODE_PASS == scan_mode ||
        BTM_BLE_BATCH_SCAN_MODE_ACTI == scan_mode)
        && (BTM_BLE_SCAN_ENABLED_STATE == ble_batchscan_cb.cur_state ||
            BTM_BLE_SCAN_ENABLE_CALLED == ble_batchscan_cb.cur_state))
    {
        status = btm_ble_read_batchscan_reports(scan_mode);
        if(BTM_CMD_STARTED == status)
        {
            /* The user needs to be provided scan read reports event */
            btm_ble_batchscan_enq_op_q(BTM_BLE_BATCH_SCAN_READ_RESULTS, ble_batchscan_cb.cur_state,
                                       BTM_BLE_BATCH_SCAN_READ_REPTS_EVT, ref_value);
        }
    }
    else
    {
        BTM_TRACE_ERROR("Illegal read scan params: %d, %d, %d", read_scan_mode, scan_mode,
            ble_batchscan_cb.cur_state);
        return BTM_ILLEGAL_VALUE;
    }
    return status;
}


/*******************************************************************************
**
** Function         btm_ble_batchscan_init
**
** Description      This function initialize the batch scan control block.
**
** Parameters       None
**
** Returns          status
**
*******************************************************************************/
void btm_ble_batchscan_init(void)
{
    BTM_TRACE_EVENT (" btm_ble_batchscan_init");
    memset(&ble_batchscan_cb, 0, sizeof(tBTM_BLE_BATCH_SCAN_CB));
    BTM_RegisterForVSEvents(btm_ble_batchscan_filter_track_adv_vse_cback, TRUE);
}

#endif