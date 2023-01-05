/*
 * Note: this file originally auto-generated by mib2c
 * using mfd-data-get.m2c
 *
 * @file applTable_data_get.h
 *
 * @addtogroup get
 *
 * Prototypes for get functions
 *
 * @{
 */
#ifndef APPLTABLE_DATA_GET_H
#define APPLTABLE_DATA_GET_H

#ifdef __cplusplus
extern          "C" {
#endif

    /*
     *********************************************************************
     * GET function declarations
     */

    /*
     *********************************************************************
     * GET Table declarations
     */
/**********************************************************************
 **********************************************************************
 ***
 *** Table applTable
 ***
 **********************************************************************
 **********************************************************************/
    /*
     * NETWORK-SERVICES-MIB::applTable is subid 1 of application.
     * Its status is Current.
     * OID: .1.3.6.1.2.1.27.1, length: 8
     */
    /*
     * indexes
     */

    int             applName_get(applTable_rowreq_ctx * rowreq_ctx,
                                 char **applName_val_ptr_ptr,
                                 size_t *applName_val_ptr_len_ptr);
    int             applDirectoryName_get(applTable_rowreq_ctx *
                                          rowreq_ctx,
                                          char
                                          **applDirectoryName_val_ptr_ptr,
                                          size_t
                                          *applDirectoryName_val_ptr_len_ptr);
    int             applVersion_get(applTable_rowreq_ctx * rowreq_ctx,
                                    char **applVersion_val_ptr_ptr,
                                    size_t *applVersion_val_ptr_len_ptr);
    int             applUptime_get(applTable_rowreq_ctx * rowreq_ctx,
                                   u_long * applUptime_val_ptr);
    int             applOperStatus_get(applTable_rowreq_ctx * rowreq_ctx,
                                       u_long * applOperStatus_val_ptr);
    int             applLastChange_get(applTable_rowreq_ctx * rowreq_ctx,
                                       u_long * applLastChange_val_ptr);
    int             applInboundAssociations_get(applTable_rowreq_ctx *
                                                rowreq_ctx,
                                                u_long *
                                                applInboundAssociations_val_ptr);
    int             applOutboundAssociations_get(applTable_rowreq_ctx *
                                                 rowreq_ctx,
                                                 u_long *
                                                 applOutboundAssociations_val_ptr);
    int            
        applAccumulatedInboundAssociations_get(applTable_rowreq_ctx *
                                               rowreq_ctx,
                                               u_long *
                                               applAccumulatedInboundAssociations_val_ptr);
    int            
        applAccumulatedOutboundAssociations_get(applTable_rowreq_ctx *
                                                rowreq_ctx,
                                                u_long *
                                                applAccumulatedOutboundAssociations_val_ptr);
    int             applLastInboundActivity_get(applTable_rowreq_ctx *
                                                rowreq_ctx,
                                                u_long *
                                                applLastInboundActivity_val_ptr);
    int             applLastOutboundActivity_get(applTable_rowreq_ctx *
                                                 rowreq_ctx,
                                                 u_long *
                                                 applLastOutboundActivity_val_ptr);
    int            
        applRejectedInboundAssociations_get(applTable_rowreq_ctx *
                                            rowreq_ctx,
                                            u_long *
                                            applRejectedInboundAssociations_val_ptr);
    int             applFailedOutboundAssociations_get(applTable_rowreq_ctx
                                                       * rowreq_ctx,
                                                       u_long *
                                                       applFailedOutboundAssociations_val_ptr);
    int             applDescription_get(applTable_rowreq_ctx * rowreq_ctx,
                                        char **applDescription_val_ptr_ptr,
                                        size_t
                                        *applDescription_val_ptr_len_ptr);
    int             applURL_get(applTable_rowreq_ctx * rowreq_ctx,
                                char **applURL_val_ptr_ptr,
                                size_t *applURL_val_ptr_len_ptr);


    int             applTable_indexes_set_tbl_idx(applTable_mib_index *
                                                  tbl_idx,
                                                  long applIndex_val);
    int             applTable_indexes_set(applTable_rowreq_ctx *
                                          rowreq_ctx, long applIndex_val);




#ifdef __cplusplus
}
#endif
#endif                          /* APPLTABLE_DATA_GET_H */
/** @} */
