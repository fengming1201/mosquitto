/*
Copyright (c) 2010-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#ifdef WITH_PERSISTENCE

#ifndef WIN32
#include <arpa/inet.h>
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "persist.h"
#include "time_mosq.h"
#include "util_mosq.h"

static uint32_t db_version;

const unsigned char magic[15] = {0x00, 0xB5, 0x00, 'm','o','s','q','u','i','t','t','o',' ','d','b'};

static int persist__restore_sub(struct mosquitto_db *db, const char *client_id, const char *sub, int qos);
int persist__read_string(FILE *db_fptr, char **str);

static struct mosquitto *persist__find_or_add_context(struct mosquitto_db *db, const char *client_id, uint16_t last_mid)
{
	struct mosquitto *context;

	context = NULL;
	HASH_FIND(hh_id, db->contexts_by_id, client_id, strlen(client_id), context);
	if(!context){
		context = context__init(db, -1);
		if(!context) return NULL;
		context->id = mosquitto__strdup(client_id);
		if(!context->id){
			mosquitto__free(context);
			return NULL;
		}

		context->clean_start = false;

		HASH_ADD_KEYPTR(hh_id, db->contexts_by_id, context->id, strlen(context->id), context);
	}
	if(last_mid){
		context->last_mid = last_mid;
	}
	return context;
}


int persist__read_string(FILE *db_fptr, char **str)
{
	uint16_t i16temp;
	uint16_t slen;
	char *s = NULL;

	if(fread(&i16temp, 1, sizeof(uint16_t), db_fptr) != sizeof(uint16_t)){
		return MOSQ_ERR_INVAL;
	}

	slen = ntohs(i16temp);
	if(slen){
		s = mosquitto__malloc(slen+1);
		if(!s){
			fclose(db_fptr);
			log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		if(fread(s, 1, slen, db_fptr) != slen){
			mosquitto__free(s);
			return MOSQ_ERR_NOMEM;
		}
		s[slen] = '\0';
	}

	*str = s;
	return MOSQ_ERR_SUCCESS;
}


static int persist__client_msg_restore(struct mosquitto_db *db, const char *client_id, uint16_t mid, uint8_t qos, uint8_t retain, uint8_t direction, uint8_t state, uint8_t dup, uint64_t store_id)
{
	struct mosquitto_client_msg *cmsg;
	struct mosquitto_client_msg **msgs, **last_msg;
	struct mosquitto_msg_store_load *load;
	struct mosquitto *context;

	cmsg = mosquitto__malloc(sizeof(struct mosquitto_client_msg));
	if(!cmsg){
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}

	cmsg->next = NULL;
	cmsg->store = NULL;
	cmsg->mid = mid;
	cmsg->qos = qos;
	cmsg->retain = retain;
	cmsg->timestamp = 0;
	cmsg->direction = direction;
	cmsg->state = state;
	cmsg->dup = dup;

	HASH_FIND(hh, db->msg_store_load, &store_id, sizeof(dbid_t), load);
	if(!load){
		mosquitto__free(cmsg);
		log__printf(NULL, MOSQ_LOG_ERR, "Error restoring persistent database, message store corrupt.");
		return 1;
	}
	cmsg->store = load->store;
	cmsg->store->ref_count++;

	context = persist__find_or_add_context(db, client_id, 0);
	if(!context){
		mosquitto__free(cmsg);
		log__printf(NULL, MOSQ_LOG_ERR, "Error restoring persistent database, message store corrupt.");
		return 1;
	}

	if (state == mosq_ms_queued){
		msgs = &(context->queued_msgs);
		last_msg = &(context->last_queued_msg);
	}else{
		msgs = &(context->inflight_msgs);
		last_msg = &(context->last_inflight_msg);
	}
	if(*msgs){
		(*last_msg)->next = cmsg;
	}else{
		*msgs = cmsg;
	}
	*last_msg = cmsg;

	return MOSQ_ERR_SUCCESS;
}


static int persist__client_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	int rc = 0;
	struct mosquitto *context;
	struct P_client chunk;

	memset(&chunk, 0, sizeof(struct P_client));

	rc = persist__client_chunk_read_v234(db_fptr, &chunk, db_version);
	if(rc){
		fclose(db_fptr);
		return rc;
	}

	context = persist__find_or_add_context(db, chunk.client_id, chunk.F.last_mid);
	if(context){
		context->disconnect_t = chunk.F.disconnect_t;
	}else{
		rc = 1;
	}

	mosquitto__free(chunk.client_id);

	return rc;
}


static int persist__client_msg_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	struct P_client_msg chunk;
	int rc;

	memset(&chunk, 0, sizeof(struct P_client_msg));

	rc = persist__client_msg_chunk_read_v234(db_fptr, &chunk);
	if(rc){
		fclose(db_fptr);
		return rc;
	}

	rc = persist__client_msg_restore(db, chunk.client_id, chunk.F.mid, chunk.F.qos,
			chunk.F.retain, chunk.F.direction, chunk.F.state, chunk.F.dup, chunk.F.store_id);
	mosquitto__free(chunk.client_id);

	return rc;
}


static int persist__msg_store_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	struct P_msg_store chunk;
	struct mosquitto_msg_store *stored = NULL;
	struct mosquitto_msg_store_load *load;
	int rc = 0;
	int i;

	memset(&chunk, 0, sizeof(struct P_msg_store));

	rc = persist__msg_store_chunk_read_v234(db_fptr, &chunk, db_version);
	if(rc){
		fclose(db_fptr);
		return rc;
	}

	if(chunk.F.source_port){
		for(i=0; i<db->config->listener_count; i++){
			if(db->config->listeners[i].port == chunk.F.source_port){
				chunk.source.listener = &db->config->listeners[i];
				break;
			}
		}
	}
	load = mosquitto__malloc(sizeof(struct mosquitto_msg_store_load));
	if(!load){
		fclose(db_fptr);
		mosquitto__free(chunk.source.id);
		mosquitto__free(chunk.source.username);
		mosquitto__free(chunk.topic);
		UHPA_FREE(chunk.payload, chunk.F.payloadlen);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}

	rc = db__message_store(db, &chunk.source, chunk.F.source_mid,
			chunk.topic, chunk.F.qos, chunk.F.payloadlen,
			&chunk.payload, chunk.F.retain, &stored, 0, NULL, chunk.F.store_id);

	mosquitto__free(chunk.source.id);
	mosquitto__free(chunk.source.username);
	chunk.source.id = NULL;
	chunk.source.username = NULL;

	if(rc == MOSQ_ERR_SUCCESS){
		load->db_id = stored->db_id;
		load->store = stored;

		HASH_ADD(hh, db->msg_store_load, db_id, sizeof(dbid_t), load);
		return MOSQ_ERR_SUCCESS;
	}else{
		mosquitto__free(load);
		fclose(db_fptr);
		return rc;
	}
}

static int persist__retain_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	struct mosquitto_msg_store_load *load;
	struct P_retain chunk;
	int rc;

	memset(&chunk, 0, sizeof(struct P_retain));

	rc = persist__retain_chunk_read_v234(db_fptr, &chunk);
	if(rc){
		fclose(db_fptr);
		return rc;
	}

	HASH_FIND(hh, db->msg_store_load, &chunk.F.store_id, sizeof(dbid_t), load);
	if(load){
		sub__messages_queue(db, NULL, load->store->topic, load->store->qos, load->store->retain, &load->store);
	}else{
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Corrupt database whilst restoring a retained message.");
		return MOSQ_ERR_INVAL;
	}
	return MOSQ_ERR_SUCCESS;
}

static int persist__sub_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	struct P_sub chunk;
	int rc;

	memset(&chunk, 0, sizeof(struct P_sub));

	rc = persist__sub_chunk_read_v234(db_fptr, &chunk);
	if(rc){
		fclose(db_fptr);
		return rc;
	}

	rc = persist__restore_sub(db, chunk.client_id, chunk.topic, chunk.F.qos);

	mosquitto__free(chunk.client_id);
	mosquitto__free(chunk.topic);

	return rc;
}


int persist__chunk_header_read(FILE *db_fptr, int *chunk, int *length)
{
	return persist__chunk_header_read_v234(db_fptr, chunk, length);
}


int persist__restore(struct mosquitto_db *db)
{
	FILE *fptr;
	char header[15];
	int rc = 0;
	uint32_t crc;
	dbid_t i64temp;
	uint32_t i32temp;
	int chunk, length;
	uint8_t i8temp;
	ssize_t rlen;
	char *err;
	struct mosquitto_msg_store_load *load, *load_tmp;

	assert(db);
	assert(db->config);

	if(!db->config->persistence || db->config->persistence_filepath == NULL){
		return MOSQ_ERR_SUCCESS;
	}

	db->msg_store_load = NULL;

	fptr = mosquitto__fopen(db->config->persistence_filepath, "rb", false);
	if(fptr == NULL) return MOSQ_ERR_SUCCESS;
	rlen = fread(&header, 1, 15, fptr);
	if(rlen == 0){
		fclose(fptr);
		log__printf(NULL, MOSQ_LOG_WARNING, "Warning: Persistence file is empty.");
		return 0;
	}else if(rlen != 15){
		goto error;
	}
	if(!memcmp(header, magic, 15)){
		// Restore DB as normal
		read_e(fptr, &crc, sizeof(uint32_t));
		read_e(fptr, &i32temp, sizeof(uint32_t));
		db_version = ntohl(i32temp);
		/* IMPORTANT - this is where compatibility checks are made.
		 * Is your DB change still compatible with previous versions?
		 */
		if(db_version > MOSQ_DB_VERSION && db_version != 0){
			if(db_version == 3){
				/* Addition of source_username and source_port to msg_store chunk in v4, v1.5.6 */
			}else if(db_version == 2){
				/* Addition of disconnect_t to client chunk in v3. */
			}else{
				fclose(fptr);
				log__printf(NULL, MOSQ_LOG_ERR, "Error: Unsupported persistent database format version %d (need version %d).", db_version, MOSQ_DB_VERSION);
				return 1;
			}
		}

		while(persist__chunk_header_read(fptr, &chunk, &length) == MOSQ_ERR_SUCCESS){
			switch(chunk){
				case DB_CHUNK_CFG:
					read_e(fptr, &i8temp, sizeof(uint8_t)); // shutdown
					read_e(fptr, &i8temp, sizeof(uint8_t)); // sizeof(dbid_t)
					if(i8temp != sizeof(dbid_t)){
						log__printf(NULL, MOSQ_LOG_ERR, "Error: Incompatible database configuration (dbid size is %d bytes, expected %lu)",
								i8temp, (unsigned long)sizeof(dbid_t));
						fclose(fptr);
						return 1;
					}
					read_e(fptr, &i64temp, sizeof(dbid_t));
					db->last_db_id = i64temp;
					break;

				case DB_CHUNK_MSG_STORE:
					if(persist__msg_store_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_CLIENT_MSG:
					if(persist__client_msg_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_RETAIN:
					if(persist__retain_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_SUB:
					if(persist__sub_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_CLIENT:
					if(persist__client_chunk_restore(db, fptr)) return 1;
					break;

				default:
					log__printf(NULL, MOSQ_LOG_WARNING, "Warning: Unsupported chunk \"%d\" in persistent database file. Ignoring.", chunk);
					fseek(fptr, length, SEEK_CUR);
					break;
			}
		}
		if(rlen < 0) goto error;
	}else{
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Unable to restore persistent database. Unrecognised file format.");
		rc = 1;
	}

	fclose(fptr);

	HASH_ITER(hh, db->msg_store_load, load, load_tmp){
		HASH_DELETE(hh, db->msg_store_load, load);
		mosquitto__free(load);
	}
	return rc;
error:
	err = strerror(errno);
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
	if(fptr) fclose(fptr);
	return 1;
}

static int persist__restore_sub(struct mosquitto_db *db, const char *client_id, const char *sub, int qos)
{
	struct mosquitto *context;

	assert(db);
	assert(client_id);
	assert(sub);

	context = persist__find_or_add_context(db, client_id, 0);
	if(!context) return 1;
	/* FIXME - identifer, options need saving */
	return sub__add(db, context, sub, qos, 0, 0, &db->subs);
}

#endif