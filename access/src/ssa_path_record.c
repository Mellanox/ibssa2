/*
 * Copyright 2004-2015 Mellanox Technologies LTD. All rights reserved.
 *
 * This software is available to you under the terms of the
 * OpenIB.org BSD license included below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <assert.h>
#include <infiniband/ssa_db.h>
#include <infiniband/ssa_smdb.h>
#include <infiniband/ssa_prdb.h>
#include <infiniband/ssa_path_record.h>
#include <ssa_path_record_helper.h>
#include <ssa_path_record_data.h>

#ifndef MIN
#define MIN(X,Y) ((X) < (Y) ?  (X) : (Y))
#endif
#ifndef MAX
#define MAX(X,Y) ((X) > (Y) ?  (X) : (Y))
#endif

#define MAX_HOPS 64
#define PK_DEFAULT_VAL ntohs(0xffff)
#define SL_DEFAULT_VAL 0

struct ssa_pr_context {
	struct ssa_pr_smdb_index *p_index;
};

struct prdb_prm {
	struct ssa_db *prdb;
	uint64_t max_count;
};

static
ssa_pr_status_t ssa_pr_path_params(const struct ssa_db *p_ssa_db_smdb,
				   const struct ssa_pr_context *p_context,
				   const struct smdb_guid2lid *p_source_rec,
				   const struct smdb_guid2lid *p_dest_rec,
				   ssa_path_parms_t *p_path_prm);

inline static size_t get_dataset_count(const struct ssa_db *p_ssa_db_smdb,
				       unsigned int table_id)
{
	SSA_ASSERT(p_ssa_db_smdb);
	SSA_ASSERT(table_id < SMDB_TBL_ID_MAX);
	SSA_ASSERT(&p_ssa_db_smdb->p_db_tables[table_id]);

	return ntohll(p_ssa_db_smdb->p_db_tables[table_id].set_count);
}

static int insert_pr_to_prdb(const ssa_path_parms_t *p_path_prm, void *prm)
{
	struct prdb_prm *p_prm;
	struct db_dataset *p_dataset = NULL;
	uint64_t set_size = 0, set_count = 0;
	struct prdb_pr *p_rec = NULL;

	p_prm = (struct prdb_prm*)prm;
	SSA_ASSERT(p_prdb);

	p_dataset = p_prm->prdb->p_db_tables + PRDB_TBL_ID_PR;
	SSA_ASSERT(p_dataset);

	set_size = ntohll(p_dataset->set_size);
	set_count = ntohll(p_dataset->set_count);

	if (set_count >= p_prm->max_count) {
		SSA_PR_LOG_INFO("PRDB is full");
		return 1;
	}

	p_rec = ((struct prdb_pr *)p_prm->prdb->pp_tables[PRDB_TBL_ID_PR]) + set_count;
	SSA_ASSERT(p_rec);

	p_rec->guid = p_path_prm->to_guid;
	p_rec->lid = p_path_prm->to_lid;
	p_rec->pk = p_path_prm->pkey;
	p_rec->mtu = p_path_prm->mtu;
	p_rec->rate = p_path_prm->rate;
	p_rec->sl = p_path_prm->sl;
	p_rec->is_reversible = p_path_prm->reversible;

	set_size += sizeof(struct prdb_pr);
	set_count++;

	p_dataset->set_count = htonll(set_count);
	p_dataset->set_size = htonll(set_size);

	return 0;
}

ssa_pr_status_t ssa_pr_half_world(struct ssa_db *p_ssa_db_smdb, void *p_ctnx,
				  be64_t port_guid,
				  ssa_pr_path_dump_t dump_clbk, void *clbk_prm)
{
	const struct smdb_guid2lid *p_source_rec = NULL;
	size_t guid_to_lid_count = 0;
	const struct smdb_guid2lid *p_guid2lid_tbl = NULL;
	size_t i = 0;
	uint16_t source_base_lid = 0;
	uint16_t source_last_lid = 0;
	uint16_t source_lid = 0;
	struct ssa_pr_context *p_context = (struct ssa_pr_context *)p_ctnx;
	int rt;

	SSA_ASSERT(port_guid);
	SSA_ASSERT(p_ssa_db_smdb);
	SSA_ASSERT(p_context);

	if (ssa_pr_rebuild_indexes(p_context->p_index, p_ssa_db_smdb)) {
		SSA_PR_LOG_ERROR("Index rebuild failed");
		return SSA_PR_ERROR;
	}

	if (!is_port_exist(p_ssa_db_smdb, port_guid)) {
		SSA_PR_LOG_ERROR("Port does not exist");
		return SSA_PR_PORT_ABSENT;
	}

	p_guid2lid_tbl = (const struct smdb_guid2lid *)
		p_ssa_db_smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	SSA_ASSERT(p_guid2lid_tbl);

	guid_to_lid_count = get_dataset_count(p_ssa_db_smdb, SMDB_TBL_ID_GUID2LID);

	p_source_rec = find_guid_to_lid_rec_by_guid(p_ssa_db_smdb, port_guid);
	if (NULL == p_source_rec) {
		SSA_PR_LOG_ERROR("GUID to LID record not found. GUID: 0x%016" PRIx64,
				 ntohll(port_guid));
		return SSA_PR_ERROR;
	}

	source_base_lid = ntohs(p_source_rec->lid);
	source_last_lid = source_base_lid + (0x01 << p_source_rec->lmc) - 1;

	for (source_lid = source_base_lid; source_lid <= source_last_lid; ++source_lid) {
		for (i = 0; i < guid_to_lid_count; i++) {
			uint16_t dest_base_lid = 0;
			uint16_t dest_last_lid = 0;
			uint16_t dest_lid = 0;

			const struct smdb_guid2lid *p_dest_rec = p_guid2lid_tbl + i;
			dest_base_lid = ntohs(p_dest_rec->lid);
			dest_last_lid = dest_base_lid +
					(0x01 << p_dest_rec->lmc) - 1;

			for (dest_lid = dest_base_lid; dest_lid <= dest_last_lid; ++dest_lid) {
				ssa_path_parms_t path_prm;
				ssa_pr_status_t path_res = SSA_PR_SUCCESS;

				path_prm.from_guid = port_guid;
				path_prm.from_lid = htons(source_lid);
				path_prm.to_guid = p_dest_rec->guid;
				path_prm.to_lid = htons(dest_lid);
				path_prm.sl = SL_DEFAULT_VAL;
				path_prm.pkey = PK_DEFAULT_VAL;

				path_res = ssa_pr_path_params(p_ssa_db_smdb,
							      p_context,
							      p_source_rec,
							      p_dest_rec,
							      &path_prm);
				if (SSA_PR_SUCCESS == path_res) {
					ssa_path_parms_t revers_path_prm;
					ssa_pr_status_t revers_path_res = SSA_PR_SUCCESS;

					revers_path_prm.from_guid = path_prm.to_guid;
					revers_path_prm.from_lid = path_prm.to_lid;
					revers_path_prm.to_guid = path_prm.from_guid;
					revers_path_prm.to_lid = path_prm.from_lid;
					revers_path_prm.reversible = 1;
					revers_path_prm.sl = SL_DEFAULT_VAL;
					revers_path_prm.pkey= PK_DEFAULT_VAL;

					revers_path_res = ssa_pr_path_params(p_ssa_db_smdb,
									     p_context,
									     p_dest_rec,
									     p_source_rec,
									     &revers_path_prm);

					if (SSA_PR_ERROR == revers_path_res) {
						SSA_PR_LOG_INFO("Reverse path calculation failed. Source LID %u Destination LID: %u",
								source_lid,
								dest_lid);
					} else
						path_prm.reversible = SSA_PR_SUCCESS == revers_path_res;

					if (NULL != dump_clbk) {
						rt = dump_clbk(&path_prm, clbk_prm);
						if (rt < 0) {
							SSA_PR_LOG_ERROR("Dump callback is failed. Ret. value %d",
									rt);
							return SSA_PR_ERROR;
						} else if (rt > 0) {
							SSA_PR_LOG_INFO("Dump callback stopped processing."
									" Ret. value %d",
									rt);
							return SSA_PR_SUCCESS;
						}
					}

				} else if (SSA_PR_ERROR == path_res) {
					SSA_PR_LOG_ERROR("Path calculation failed: (%u) -> (%u) "
							 "\"Half World\" calculation stopped",
							 source_lid, dest_lid);
					return SSA_PR_ERROR;
				}
			}
		}
	}
	return SSA_PR_SUCCESS;
}

uint64_t ssa_pr_compute_pr_max_number(struct ssa_db *p_ssa_db_smdb,
				      be64_t port_guid)
{
	size_t guid_to_lid_count = 0 , i = 0;
	const struct smdb_guid2lid *p_guid2lid_tbl = NULL;
	uint64_t destination_count = 0;
	uint8_t source_lmc = 0;

	SSA_ASSERT(p_ssa_db_smdb);

	p_guid2lid_tbl = (const struct smdb_guid2lid *)
		p_ssa_db_smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	SSA_ASSERT(p_guid_to_lid_tbl);

	guid_to_lid_count = get_dataset_count(p_ssa_db_smdb,
					      SMDB_TBL_ID_GUID2LID);

	for (i = 0; i < guid_to_lid_count; i++) {
		const struct smdb_guid2lid *p_rec = p_guid2lid_tbl + i;
		if (p_rec->guid == port_guid)
			source_lmc = p_rec->lmc;
		destination_count += (0x01 << p_rec->lmc);
	}

	return destination_count * (0x01 << source_lmc);
}

ssa_pr_status_t ssa_pr_compute_half_world(struct ssa_db *p_ssa_db_smdb,
					 void *p_ctnx, be64_t port_guid,
					 struct ssa_db **pp_prdb)
{
	uint64_t records_num[PRDB_TBL_ID_MAX] = { 0 };
	ssa_pr_status_t res = SSA_PR_SUCCESS;
	struct prdb_prm prm;
	struct ssa_pr_context *p_context = (struct ssa_pr_context *)p_ctnx;

	SSA_ASSERT(p_context);

	*pp_prdb = NULL;

	if (ssa_pr_rebuild_indexes(p_context->p_index, p_ssa_db_smdb)) {
		SSA_PR_LOG_ERROR("Index rebuild failed");
		return SSA_PR_ERROR;
	}

	if (!is_port_exist(p_ssa_db_smdb, port_guid)) {
		SSA_PR_LOG_ERROR("Port does not exist");
		return SSA_PR_PORT_ABSENT;
	}

	records_num[PRDB_TBL_ID_PR] =
		ssa_pr_compute_pr_max_number(p_ssa_db_smdb, port_guid);

	/* TODO: use previous PRDB version epoch */
	*pp_prdb = ssa_prdb_create(DB_EPOCH_INVALID /* epoch */, records_num);
	if (!*pp_prdb) {
		SSA_PR_LOG_ERROR("Path record database creation failed."
				 " Number of records: %" PRIu64,
				 records_num[PRDB_TBL_ID_PR]);
		return SSA_PR_PRDB_ERROR;
	}

	prm.prdb = *pp_prdb;
	prm.max_count = records_num[PRDB_TBL_ID_PR];

	res = ssa_pr_half_world(p_ssa_db_smdb, p_ctnx, port_guid,
				insert_pr_to_prdb,&prm);
	if (SSA_PR_ERROR == res) {
		SSA_PR_LOG_ERROR("\"Half world\" calculation failed for GUID: 0x%" PRIx64,
				 ntohll(port_guid));
		goto Error;
	}
	return SSA_PR_SUCCESS;

Error:
	if (*pp_prdb) {
		ssa_db_destroy(*pp_prdb);
		*pp_prdb = NULL;
	}
	return SSA_PR_ERROR;
}

ssa_pr_status_t ssa_pr_whole_world(struct ssa_db *p_ssa_db_smdb,
				   void *context, ssa_pr_path_dump_t dump_clbk,
				   void *clbk_prm)
{
	size_t i = 0;
	const struct smdb_guid2lid *p_guid2lid_tbl = NULL;
	size_t count = 0;
	ssa_pr_status_t res = SSA_PR_SUCCESS;
	struct ssa_pr_context *p_context = (struct ssa_pr_context *)context;

	SSA_ASSERT(p_context);

	if (ssa_pr_rebuild_indexes(p_context->p_index, p_ssa_db_smdb)) {
		SSA_PR_LOG_ERROR("Index rebuild failed");
		return SSA_PR_ERROR;
	}

	SSA_ASSERT(p_ssa_db_smdb);

	p_guid2lid_tbl = (struct smdb_guid2lid *)
		p_ssa_db_smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	SSA_ASSERT(p_guid_to_lid_tbl);

	count = get_dataset_count(p_ssa_db_smdb, SMDB_TBL_ID_GUID2LID);

	for (i = 0; i < count; i++) {
		res = ssa_pr_half_world(p_ssa_db_smdb,context,
					p_guid2lid_tbl[i].guid,
					dump_clbk, clbk_prm);
		if (SSA_PR_ERROR == res) {
			SSA_PR_LOG_ERROR("\"Half world\" calculation failed for GUID: 0x%" PRIx64
					 " . \"Whole world\" calculation stopped",
					 ntohll(p_guid2lid_tbl[i].guid));
			return res;
		}
	}
	return SSA_PR_SUCCESS;
}

static inline
const struct smdb_port *get_switch_port(const struct ssa_db *p_ssa_db_smdb,
					const struct ssa_pr_smdb_index *p_index,
					const be16_t switch_lid,
					const int port_num)
{
	return find_port(p_ssa_db_smdb, p_index, switch_lid, port_num);
}

static inline
const struct smdb_port *get_host_port(const struct ssa_db *p_ssa_db_smdb,
				      const struct ssa_pr_smdb_index *p_index,
				      const be16_t lid)
{
	/*
	 * For host there is only one record in port table.
	 * Port num is not relevant
	 */
	return find_port(p_ssa_db_smdb, p_index, lid, 0);
}

static
ssa_pr_status_t ssa_pr_path_params(const struct ssa_db *p_ssa_db_smdb,
				   const struct ssa_pr_context *p_context,
				   const struct smdb_guid2lid *p_source_rec,
				   const struct smdb_guid2lid *p_dest_rec,
				   ssa_path_parms_t *p_path_prm)
{
	const struct smdb_port *source_port = NULL;
	const struct smdb_port *dest_port = NULL;
	const struct smdb_port *port = NULL;
	const struct smdb_subnet_opts *opt_rec = NULL;

	SSA_ASSERT(p_ssa_db_smdb);
	SSA_ASSERT(p_context);
	SSA_ASSERT(p_context->p_index);
	SSA_ASSERT(p_source_rec);
	SSA_ASSERT(p_dest_rec);
	SSA_ASSERT(p_path_prm);

	opt_rec =
		(const struct smdb_subnet_opts *)p_ssa_db_smdb->pp_tables[SMDB_TBL_ID_SUBNET_OPTS];
	SSA_ASSERT(opt_rec);

	if (p_source_rec->is_switch)
		source_port = get_switch_port(p_ssa_db_smdb, p_context->p_index,
					      p_source_rec->lid, 0);
	else
		source_port = get_host_port(p_ssa_db_smdb, p_context->p_index,
					    p_source_rec->lid);
	if (NULL == source_port) {
		SSA_PR_LOG_ERROR("Source port not found. Path record calculation stopped."
				 " LID: %u",
				 ntohs(p_source_rec->lid));
		return SSA_PR_ERROR;
	}

	if (p_dest_rec->is_switch)
		dest_port = get_switch_port(p_ssa_db_smdb, p_context->p_index,
					    p_dest_rec->lid, 0);
	else
		dest_port = get_host_port(p_ssa_db_smdb, p_context->p_index,
					  p_dest_rec->lid);
	if (NULL == dest_port) {
		SSA_PR_LOG_ERROR("Destination port not found. Path record calculation stopped."
				 " LID: %u",
				 ntohs(p_dest_rec->lid));
		return SSA_PR_ERROR;
	}

	p_path_prm->pkt_life = source_port == dest_port ? 0 : opt_rec[0].subnet_timeout;
	p_path_prm->mtu = source_port->mtu_cap;
	p_path_prm->rate = source_port->rate & SSA_DB_PORT_RATE_MASK;
	p_path_prm->pkt_life = 0;
	p_path_prm->hops = 0;

	if (p_source_rec->is_switch) {
		const int out_port_num = find_destination_port(p_ssa_db_smdb,
							       p_context->p_index,
							       p_source_rec->lid,
							       p_dest_rec->lid);
		if (out_port_num  < 0) {
			SSA_PR_LOG_ERROR("Failed to find outgoing port for LID: %u"
					 " on switch LID: %u. "
					 "Path record calculation stopped",
					 ntohs(p_dest_rec->lid),
					 ntohs(p_source_rec->lid));
			return SSA_PR_ERROR;
		} else if (LFT_NO_PATH == out_port_num) {
			SSA_PR_LOG_DEBUG("There is no path from LID: %u to LID: %u", 
					 ntohs(p_source_rec->lid),
					 ntohs(p_dest_rec->lid));
			return SSA_PR_NO_PATH;
		}

		port = find_port(p_ssa_db_smdb, p_context->p_index,
				 p_source_rec->lid, out_port_num);
		if (NULL == port) {
			SSA_PR_LOG_ERROR("Port not found. Path record calculation stopped."
					 " LID: %u num: %d",
					 ntohs(p_source_rec->lid),
					 out_port_num);
			return SSA_PR_ERROR;
		}
	} else {
		port = source_port;
	}

	while (port != dest_port) {
		int out_port_num = -1;
		int port_num;
		be16_t port_lid;

		port_lid = port->port_lid;
		port_num = port->port_num;
		port = find_linked_port(p_ssa_db_smdb, p_context->p_index,
					port_lid, port_num);
		if (NULL == port) {
			SSA_PR_LOG_ERROR("Port not found. Path record calculation stopped."
					 " LID: %u num: %d",
					 ntohs(port_lid), port_num);
			return SSA_PR_ERROR;
		}

		if (port == dest_port)
			break;

		if (!(port->rate & SSA_DB_PORT_IS_SWITCH_MASK)) {
			SSA_PR_LOG_ERROR("Error: Internal error, bad path while routing "
					 "(GUID: 0x%016" PRIx64 ") port %d to "
					 "(GUID: 0x%016" PRIx64 ") port %d; "
					 "ended at (LID: %u) port %d",
					 ntohll(p_source_rec->guid),
					 source_port->port_num,
					 ntohll(p_dest_rec->guid),
					 dest_port->port_num,
					 ntohs(port->port_lid),
					 port->port_num);
			return SSA_PR_ERROR;
		}

		p_path_prm->mtu = MIN(p_path_prm->mtu,port->mtu_cap);
		if (ib_path_compare_rates_fast(p_path_prm->rate,port->rate & SSA_DB_PORT_RATE_MASK) > 0)
			p_path_prm->rate = port->rate & SSA_DB_PORT_RATE_MASK;

		out_port_num  = find_destination_port(p_ssa_db_smdb,
						      p_context->p_index,
						      port->port_lid,
						      p_dest_rec->lid);
		if (LFT_NO_PATH == out_port_num) {
			SSA_PR_LOG_DEBUG("There is no path from LID: %u to LID: %u",
					 ntohs(p_source_rec->lid),
					 ntohs(p_dest_rec->lid));
			return SSA_PR_NO_PATH;
		}

		port_lid = port->port_lid;
		port = find_port(p_ssa_db_smdb, p_context->p_index,
				 port_lid, out_port_num);
		if (NULL == port) {
			SSA_PR_LOG_ERROR("Port not found. Path record calculation stopped."
					 " LID: %u num: %d",
					 ntohs(port_lid), out_port_num);
			return SSA_PR_ERROR;
		}

		p_path_prm->mtu = MIN(p_path_prm->mtu,port->mtu_cap);
		if (ib_path_compare_rates_fast(p_path_prm->rate,port->rate & SSA_DB_PORT_RATE_MASK) > 0)
			p_path_prm->rate = port->rate & SSA_DB_PORT_RATE_MASK;
		p_path_prm->hops++;

		if (p_path_prm->hops > MAX_HOPS) {
			SSA_PR_LOG_ERROR(
				"Path from GUID 0x%016" PRIx64 " (port %d) "
				"to lid %u GUID 0x%016" PRIx64 " (port %d) "
				"needs more than %d hops, max %d hops allowed",
				ntohll(p_source_rec->guid),
				source_port->port_num, ntohs(p_dest_rec->lid),
				ntohll(p_dest_rec->guid), dest_port->port_num,
				p_path_prm->hops, MAX_HOPS);
			return SSA_PR_ERROR;
		}
	}

	p_path_prm->mtu = MIN(p_path_prm->mtu, port->mtu_cap);
	if(ib_path_compare_rates_fast(p_path_prm->rate,port->rate & SSA_DB_PORT_RATE_MASK) > 0)
		p_path_prm->rate = port->rate & SSA_DB_PORT_RATE_MASK;

	return SSA_PR_SUCCESS;
}

void ssa_pr_reinit_context(void *context, struct ssa_db *smdb)
{
	struct ssa_pr_context *p_context = context;

	ssa_pr_destroy_indexes(p_context->p_index);
	ssa_pr_build_indexes(p_context->p_index, smdb);
}

void *ssa_pr_create_context()
{
	struct ssa_pr_context *p_context = NULL;

	p_context = (struct ssa_pr_context *)malloc(sizeof(struct ssa_pr_context));
	if (!p_context) {
		SSA_PR_LOG_ERROR("Cannot allocate path record calculation context");
		goto Error;
	}

	memset(p_context,'\0',sizeof(struct ssa_pr_context));

	p_context->p_index = (struct ssa_pr_smdb_index *)malloc(sizeof(struct ssa_pr_smdb_index));
	if (!p_context->p_index) {
		SSA_PR_LOG_ERROR("Cannot allocate path record data index");
		goto Error;
	}

	memset(p_context->p_index,'\0',sizeof(struct ssa_pr_smdb_index));

	return p_context;

Error:
	if (p_context && p_context->p_index) {
		free(p_context->p_index);
		p_context->p_index = NULL;
	}

	if (p_context) {
		free(p_context);
		p_context = NULL;
	}

	return NULL;
}

void ssa_pr_destroy_context(void *ctx)
{
	struct ssa_pr_context *p_context = (struct ssa_pr_context *)ctx;

	if (p_context) {
		if (p_context->p_index) {
			ssa_pr_destroy_indexes(p_context->p_index);
			free(p_context->p_index);
			p_context->p_index = NULL;
		}
		free(p_context);
		p_context = NULL;
	}
}
