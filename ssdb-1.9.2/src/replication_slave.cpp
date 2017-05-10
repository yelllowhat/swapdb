//
// Created by zts on 17-5-10.
//


#include "replication.h"
#include "serv.h"
#include "util/cfree.h"

extern "C" {
#include <redis/rdb.h>
#include <redis/lzf.h>
}


void *ssdb_sync(void *arg) {
    ReplicationJob *job = (ReplicationJob *) arg;
    Context ctx = job->ctx;
    SSDBServer *serv = (SSDBServer *)ctx.net->data;
    HostAndPort hnp = job->hnp;
    Link *master_link = job->upstream;

    delete job;

    if (serv->ssdb->expiration) {
        serv->ssdb->expiration->stop();
    }
    serv->ssdb->stop();

    log_warn("[ssdb_sync] do flushdb");
    serv->ssdb->flushdb();


    log_warn("[ssdb_sync] ready to revieve");
    master_link->quick_send({"ok", "ok"});


    log_debug("[ssdb_sync] prepare for event loop");

    unique_ptr<Fdevents> fdes = unique_ptr<Fdevents>(new Fdevents());

    fdes->set(master_link->fd(), FDEVENT_IN, 1, master_link); //open evin
    master_link->noblock(true);


    const Fdevents::events_t *events;
    ready_list_t ready_list;
    ready_list_t ready_list_2;
    ready_list_t::iterator it;

    int errorCode = 0;

    std::vector<std::string> kvs;

    int ret = 0;
    bool complete = false;

    while (!job->quit) {
        ready_list.swap(ready_list_2);
        ready_list_2.clear();

        if (!ready_list.empty()) {
            // ready_list not empty, so we should return immediately
            events = fdes->wait(0);
        } else {
            events = fdes->wait(50);
        }

        if (events == nullptr) {
            log_fatal("events.wait error: %s", strerror(errno));
            //exit
            errorCode = -1;
            break;
        }

        for (int i = 0; i < (int) events->size(); i++) {
            //processing
            const Fdevent *fde = events->at(i);
            Link *link = (Link *) fde->data.ptr;
            if (fde->events & FDEVENT_IN) {
                ready_list.push_back(link);
                if (link->error()) {
                    continue;
                }
                int len = link->read(256 * 1024);
                if (len <= 0) {
                    log_debug("fd: %d, read: %d, delete link, e:%d, f:%d", link->fd(), len, fde->events, fde->s_flags);
                    link->mark_error();
                    continue;
                }
            }
            if (fde->events & FDEVENT_OUT) {
                if (link->output->empty()) {
                    fdes->clr(link->fd(), FDEVENT_OUT);
                    continue;
                }

                if (link->error()) {
                    continue;
                }
                int len = link->write();
                if (len <= 0) {
//                if (len < 0) {
                    log_debug("fd: %d, write: %d, delete link, e:%d, f:%d", link->fd(), len, fde->events, fde->s_flags);
                    link->mark_error();
                    continue;
                }
                if (link->output->empty()) {
                    fdes->clr(link->fd(), FDEVENT_OUT);
                }
            }
        }


        for (it = ready_list.begin(); it != ready_list.end(); it++) {
            Link *link = *it;
            if (link->error()) {
                log_warn("fd: %d, link broken, address:%lld", link->fd(), link);
                //TODO
                errorCode = -1;
                break;
            }

            while (link->input->size() > 1) {
                Decoder decoder(link->input->data(), link->input->size());

                int oper_offset = 0, raw_size_offset = 0, compressed_offset = 0;
                uint64_t oper_len = 0, raw_len = 0, compressed_len = 0;

                if (replic_decode_len(decoder.data(), &oper_offset, &oper_len) == -1) {
                    errorCode = -1;
                    break;
                }
                decoder.skip(oper_offset);

                if (decoder.size() < ((int) oper_len)) {
                    link->input->grow();
                    break;
                }

                std::string oper(decoder.data(), oper_len);
                decoder.skip((int) oper_len);

                if (oper == "mset") {

                    if (decoder.size() < 1) {
                        link->input->grow();
                        break;
                    }
                    if (replic_decode_len(decoder.data(), &raw_size_offset, &raw_len) == -1) {
                        errorCode = -1;
                        break;
                    }
                    decoder.skip(raw_size_offset);

                    if (decoder.size() < 1) {
                        link->input->grow();
                        break;
                    }
                    if (replic_decode_len(decoder.data(), &compressed_offset, &compressed_len) == -1) {
                        errorCode = -1;
                        break;
                    }
                    decoder.skip(compressed_offset);

                    if (decoder.size() < (compressed_len)) {
                        link->input->grow();
                        break;
                    }

                    std::unique_ptr<char, cfree_delete<char>> t_val((char *) malloc(raw_len));

                    if (lzf_decompress(decoder.data(), compressed_len, t_val.get(), raw_len) == 0) {
                        errorCode = -1;
                        break;;
                    }

                    Decoder decoder_item(t_val.get(), raw_len);

                    uint64_t remian_length = raw_len;
                    while (remian_length > 0) {
                        int key_offset = 0, val_offset = 0;
                        uint64_t key_len = 0, val_len = 0;

                        if (replic_decode_len(decoder_item.data(), &key_offset, &key_len) == -1) {
                            errorCode = -1;
                            break;
                        }
                        decoder_item.skip(key_offset);
                        std::string key(decoder_item.data(), key_len);
                        decoder_item.skip((int) key_len);
                        remian_length -= (key_offset + (int) key_len);

                        if (replic_decode_len(decoder_item.data(), &val_offset, &val_len) == -1) {
                            errorCode = -1;
                            break;
                        }
                        decoder_item.skip(val_offset);
                        std::string value(decoder_item.data(), val_len);
                        decoder_item.skip((int) val_len);
                        remian_length -= (val_offset + (int) val_len);

                        kvs.push_back(key);
                        kvs.push_back(value);
                    }

                    decoder.skip(compressed_len);
                    link->input->decr(link->input->size() - decoder.size());

                    if (!kvs.empty()) {
                        log_debug("parse_replic count %d", kvs.size());
                        errorCode = serv->ssdb->parse_replic(ctx, kvs);
                        kvs.clear();
                    }

                } else if (oper == "complete") {
                    link->input->decr(link->input->size() - decoder.size());
                    complete = true;
                    job->quit = true;
                } else {
                    log_error("unknown oper code");
                    errorCode = -1;
                    break;
                }
            }

            if (errorCode != 0) {
                break;
            }
        }

        if (errorCode != 0) {
            break;
        }

    }


    if (!kvs.empty()) {
        log_debug("parse_replic count %d", kvs.size());
        errorCode = serv->ssdb->parse_replic(ctx, kvs);
        kvs.clear();
    }


    if (errorCode != 0) {
        log_error("[ssdb_sync] recieve snapshot from %s:%d failed!", hnp.ip.c_str(), hnp.port);
    } else {
        master_link->quick_send({"ok"});
        log_info("[ssdb_sync] recieve snapshot from %s:%d finished!", hnp.ip.c_str(), hnp.port);
    }

    delete master_link;

    if (serv->ssdb->expiration != nullptr) {
        serv->ssdb->expiration->start();
    }
    serv->ssdb->start();


    return (void *) NULL;
}


int replic_decode_len(const char *data, int *offset, uint64_t *lenptr) {
    unsigned char buf[2];
    buf[0] = (unsigned char) data[0];
    buf[1] = (unsigned char) data[1];
    int type;
    type = (buf[0] & 0xC0) >> 6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        *lenptr = buf[0] & 0x3F;
        *offset = 1;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0] & 0x3F;
        *offset = 1;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        *lenptr = ((buf[0] & 0x3F) << 8) | buf[1];
        *offset = 2;
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        len = *(uint32_t *) (data + 1);
        *lenptr = be32toh(len);
        *offset = 1 + sizeof(uint32_t);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        len = *(uint64_t *) (data + 1);
        *lenptr = be64toh(len);
        *offset = 1 + sizeof(uint64_t);
    } else {
        printf("Unknown length encoding %d in rdbLoadLen()", type);
        return -1; /* Never reached. */
    }
    return 0;
}
