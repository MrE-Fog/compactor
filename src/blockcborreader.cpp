/*
 * Copyright 2016-2019 Internet Corporation for Assigned Names and Numbers.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Developed by Sinodun IT (www.sinodun.com)
 */

#include <algorithm>
#include <iomanip>
#include <vector>

#include "config.h"

#include "baseoutputwriter.hpp"
#include "blockcbor.hpp"
#include "bytestring.hpp"
#include "makeunique.hpp"
#include "dnsmessage.hpp"

#include "blockcborreader.hpp"

BlockCborReader::BlockCborReader(CborBaseDecoder& dec,
                                 Configuration& config,
                                 const Defaults& defaults,
                                 boost::optional<PseudoAnonymise> pseudo_anon)
    : dec_(dec), next_item_(0), need_block_(true),
      file_format_version_(block_cbor::FileFormatVersion::format_10),
      current_block_num_(0),
      defaults_(defaults),
      pseudo_anon_(pseudo_anon)
{
    readFileHeader(config);
    block_ = make_unique<block_cbor::BlockData>(block_parameters_, file_format_version_);
}

void BlockCborReader::readFileHeader(Configuration& config)
{
    try
    {
        bool old_no_header_format = true;

        // Initial array header.
        bool indef;
        uint64_t n_elems = dec_.readArrayHeader(indef);
        if ( dec_.type() == CborBaseDecoder::TYPE_STRING )
            old_no_header_format = false;

        if ( !old_no_header_format )
        {
            if ( n_elems != 3 )
                throw cbor_file_format_error("Unexpected initial array length");

            std::string file_type_id = dec_.read_string();
            if ( file_type_id == block_cbor::FILE_FORMAT_ID )
                readFilePreamble(config, block_cbor::FileFormatVersion::format_10);
            else if ( file_type_id == block_cbor::FILE_FORMAT_02_ID )
                readFilePreamble(config, block_cbor::FileFormatVersion::format_02);
            else
                throw cbor_file_format_error("This is not a C-DNS file");

            // Finally, the start of the block array.
            nblocks_ = dec_.readArrayHeader(blocks_indef_);
        }
        else
        {
            nblocks_ = n_elems;
            blocks_indef_ = indef;
            fields_ = make_unique<block_cbor::FileVersionFields>(0, block_cbor::FILE_FORMAT_02_VERSION, 0);
        }
    }
    catch (const std::logic_error& e)
    {
        throw cbor_file_format_error("Unexpected item reading header");
    }
}

void BlockCborReader::readFilePreamble(Configuration& config, block_cbor::FileFormatVersion header_version)
{
    unsigned major_version = 0;
    unsigned minor_version = 0;
    unsigned private_version = 0;
    bool indef;
    uint64_t n_elems = dec_.readMapHeader(indef);
    while ( indef || n_elems-- > 0 )
    {
        if ( indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
        {
            dec_.readBreak();
            break;
        }

        switch(block_cbor::file_preamble_field(dec_.read_unsigned(), header_version))
        {
        case block_cbor::FilePreambleField::major_format_version:
            if ( header_version != block_cbor::FileFormatVersion::format_10 )
                throw cbor_file_format_error("Unexpected version item reading header");
            major_version = dec_.read_unsigned();
            break;

        case block_cbor::FilePreambleField::minor_format_version:
            if ( header_version != block_cbor::FileFormatVersion::format_10 )
                throw cbor_file_format_error("Unexpected version item reading header");
            minor_version = dec_.read_unsigned();
            break;

        case block_cbor::FilePreambleField::private_version:
            if ( header_version != block_cbor::FileFormatVersion::format_10 )
                throw cbor_file_format_error("Unexpected version item reading header");
            private_version = dec_.read_unsigned();
            break;

            // This may be either format 1.0 block parameters,
            // format 0.5 configuration or format 0.2 configuration.
            // Now we can distinguish format 1.0 and format 0.5, and
            // at last set up the field mapper.
        case block_cbor::FilePreambleField::block_parameters:
        case block_cbor::FilePreambleField::configuration:
            if ( header_version == block_cbor::FileFormatVersion::format_10 &&
                 major_version == block_cbor::FILE_FORMAT_05_MAJOR_VERSION &&
                 minor_version == block_cbor::FILE_FORMAT_05_MINOR_VERSION )
            {
                header_version = block_cbor::FileFormatVersion::format_05;
                file_format_version_ = header_version;
            }

            if ( fields_ )
                throw cbor_file_format_error("Unexpected configuration reading header");
            fields_ = make_unique<block_cbor::FileVersionFields>(major_version, minor_version, private_version);

            switch(header_version)
            {
            case block_cbor::FileFormatVersion::format_10:
                readBlockParameters(config);
                break;

            case block_cbor::FileFormatVersion::format_05:
            case block_cbor::FileFormatVersion::format_02:
                readConfiguration(config);
                break;

            default:
                throw cbor_file_format_error("Unexpected version item reading header");
                break;
            }
            break;

        // Obsolete items format 0.5
        case block_cbor::FilePreambleField::generator_id:
            generator_id_ = dec_.read_string();
            break;

        case block_cbor::FilePreambleField::host_id:
            host_id_ = dec_.read_string();
#if ENABLE_PSEUDOANONYMISATION
            if ( pseudo_anon_ )
                host_id_.clear();
#endif
            break;

        // Obsolete items format 0.2
        case block_cbor::FilePreambleField::format_version:
            if ( header_version != block_cbor::FileFormatVersion::format_02 )
                throw cbor_file_format_error("Unexpected version item reading header");
            minor_version = dec_.read_unsigned();
            if ( minor_version != block_cbor::FILE_FORMAT_02_VERSION )
                throw cbor_file_format_error("Wrong file format version");
            file_format_version_ = header_version;
            break;

        default:
            // Unknown item, skip.
            dec_.skip();
            break;
        }
    }

    if ( !fields_ )
        throw cbor_file_format_error("File preamble missing version information");
}

void BlockCborReader::readConfiguration(Configuration& config)
{
    bool indef;
    uint64_t n_elems = dec_.readMapHeader(indef);
    while ( indef || n_elems-- > 0 )
    {
        bool arr_indef;
        uint64_t arr_elems;

        if ( indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
        {
            dec_.readBreak();
            break;
        }

        switch(fields_->configuration_field(dec_.read_unsigned()))
        {
        case block_cbor::ConfigurationField::query_timeout:
            config.query_timeout = dec_.read_unsigned();
            break;

        case block_cbor::ConfigurationField::skew_timeout:
            config.skew_timeout = dec_.read_unsigned();
            break;

        case block_cbor::ConfigurationField::snaplen:
            config.snaplen = dec_.read_unsigned();
            break;

        case block_cbor::ConfigurationField::promisc:
            config.promisc_mode = dec_.read_bool();
            break;

        case block_cbor::ConfigurationField::interfaces:
            config.network_interfaces.clear();
            arr_elems = dec_.readArrayHeader(arr_indef);
            while ( arr_indef || arr_elems-- > 0 )
            {
                if ( arr_indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
                {
                    dec_.readBreak();
                    break;
                }

                config.network_interfaces.push_back(dec_.read_string());
            }
            break;

        case block_cbor::ConfigurationField::filter:
            config.filter = dec_.read_string();
#if ENABLE_PSEUDOANONYMISATION
            if ( pseudo_anon_ )
                config.filter.clear();
#endif
            break;

        case block_cbor::ConfigurationField::query_options:
            config.output_options_queries = dec_.read_unsigned();
            break;

        case block_cbor::ConfigurationField::response_options:
            config.output_options_responses = dec_.read_unsigned();
            break;

        case block_cbor::ConfigurationField::vlan_ids:
            config.vlan_ids.clear();
            arr_elems = dec_.readArrayHeader(arr_indef);
            while ( arr_indef || arr_elems-- > 0 )
            {
                if ( arr_indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
                {
                    dec_.readBreak();
                    break;
                }

                config.vlan_ids.push_back(dec_.read_unsigned());
            }
            break;

        case block_cbor::ConfigurationField::accept_rr_types:
            config.accept_rr_types.clear();
            arr_elems = dec_.readArrayHeader(arr_indef);
            while ( arr_indef || arr_elems-- > 0 )
            {
                if ( arr_indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
                {
                    dec_.readBreak();
                    break;
                }

                config.accept_rr_types.push_back(dec_.read_unsigned());
            }
            break;

        case block_cbor::ConfigurationField::ignore_rr_types:
            config.ignore_rr_types.clear();
            arr_elems = dec_.readArrayHeader(arr_indef);
            while ( arr_indef || arr_elems-- > 0 )
            {
                if ( arr_indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
                {
                    dec_.readBreak();
                    break;
                }

                config.ignore_rr_types.push_back(dec_.read_unsigned());
            }
            break;

        case block_cbor::ConfigurationField::server_addresses:
            config.server_addresses.clear();
            arr_elems = dec_.readArrayHeader(arr_indef);
            while ( arr_indef || arr_elems-- > 0 )
            {
                if ( arr_indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
                {
                    dec_.readBreak();
                    break;
                }

                IPAddress addr(dec_.read_binary());
#if ENABLE_PSEUDOANONYMISATION
                if ( pseudo_anon_ )
                    addr = pseudo_anon_->address(addr);
#endif
                config.server_addresses.push_back(addr);
            }
            break;

        case block_cbor::ConfigurationField::max_block_qr_items:
            config.max_block_items = dec_.read_unsigned();
            break;

        default:
            // Unknown item, skip.
            dec_.skip();
            break;
        }
    }

    // Generate a suitable instance of block parameters from this configuration.
    block_cbor::BlockParameters block_parameters;

    config.populate_block_parameters(block_parameters);
    block_parameters_.push_back(block_parameters);
}

void BlockCborReader::readBlockParameters(Configuration& config)
{
    bool first_bp = true;
    bool indef;
    uint64_t n_elems = dec_.readArrayHeader(indef);
    if ( !indef )
        block_parameters_.reserve(n_elems);
    while ( indef || n_elems-- > 0 )
    {
        if ( indef && dec_.type() == CborBaseDecoder::TYPE_BREAK )
        {
            dec_.readBreak();
            break;
        }
        block_cbor::BlockParameters bp;
        bp.readCbor(dec_, *fields_);
#if ENABLE_PSEUDOANONYMISATION
        if ( pseudo_anon_ )
        {
            bp.collection_parameters.host_id.clear();
            bp.collection_parameters.filter.clear();
            for ( auto& a : bp.collection_parameters.server_addresses )
                a = pseudo_anon_->address(a);
        }
#endif

        if ( first_bp )
        {
            config.set_from_block_parameters(bp);
            generator_id_ = bp.collection_parameters.generator_id;
            host_id_ = bp.collection_parameters.host_id;

            first_bp = false;
        }

        block_parameters_.push_back(bp);
    }
}

bool BlockCborReader::readBlock()
{
    if ( blocks_indef_ )
    {
        if ( dec_.type() == CborBaseDecoder::TYPE_BREAK )
        {
            dec_.readBreak();
            return false;
        }
    }
    else if ( nblocks_-- == 0 )
        return false;

    block_->clear();
    block_->readCbor(dec_, *fields_, defaults_);

    // Accumulate address events counts.
    for ( auto& aeci : block_->address_event_counts )
    {
        IPAddress addr;

        if ( !aeci.first.address )
            addr = *defaults_.ae_address;
        else
            addr = get_client_address(*aeci.first.address, aeci.first.transport_flags);

        AddressEvent ae(*aeci.first.type, addr, *aeci.first.code);
        if ( address_events_read_.find(ae) != address_events_read_.end() )
            address_events_read_[ae] += aeci.second;
        else
            address_events_read_[ae] = aeci.second;
    }

    next_item_ = 0;
    need_block_ = (block_->query_response_items.size() == next_item_);
    current_block_num_++;
    return true;
}

QueryResponseData BlockCborReader::readQRData(bool& eof)
{
    QueryResponseData res;

    eof = true;
    while ( need_block_ )
        if ( !readBlock() )
            return res;

    const block_cbor::QueryResponseItem& qri = block_->query_response_items[next_item_];
    need_block_ = (block_->query_response_items.size() == ++next_item_);

    if ( !qri.signature )
        throw cbor_file_format_error("QueryResponseItem missing signature");

    const block_cbor::QueryResponseSignature& sig = block_->query_response_signatures[qri.signature];

    res.timestamp = qri.tstamp;
    if ( qri.client_address )
        res.client_address = get_client_address(*qri.client_address, sig.qr_transport_flags);
    else
        res.client_address = defaults_.client_address;
    res.client_port = qri.client_port;
    if ( sig.server_address )
        res.server_address = get_server_address(*sig.server_address, sig.qr_transport_flags);
    else
        res.server_address = defaults_.server_address;
    res.server_port = sig.server_port;
    res.id = qri.id;
    if ( qri.qname )
        res.qname = block_->names_rdatas[*qri.qname].str;
    else
        res.qname = defaults_.query_name;
    res.qr_transport_flags = sig.qr_transport_flags;
    res.dns_flags = sig.dns_flags;
    if ( sig.query_classtype )
    {
        const block_cbor::ClassType& ct = block_->class_types[*sig.query_classtype];
        res.query_class = ct.qclass;
        res.query_type = ct.qtype;
    }
    else
    {
        res.query_class = defaults_.query_class;
        res.query_type = defaults_.query_type;
    }
    res.query_qdcount = sig.qdcount;
    res.query_ancount = sig.query_ancount;
    res.query_arcount = sig.query_arcount;
    res.query_nscount = sig.query_nscount;
    res.query_opcode = sig.query_opcode;
    res.query_edns_version = sig.query_edns_version;
    res.query_edns_payload_size = sig.query_edns_payload_size;
    if ( sig.query_opt_rdata )
        res.query_opt_rdata = block_->names_rdatas[*sig.query_opt_rdata].str;
    else
        res.query_opt_rdata = defaults_.query_opt_rdata;
    res.query_opcode = sig.query_opcode;
    res.query_size = qri.query_size;

    res.response_delay = qri.response_delay;
    res.response_rcode = sig.response_rcode;
    res.response_size = qri.response_size;

    if ( qri.query_extra_info )
    {
    }

    if ( qri.response_extra_info )
    {
    }

    return res;
}

void BlockCborReader::read_extra_info(
    std::unique_ptr<block_cbor::QueryResponseExtraInfo>& extra_info,
    boost::optional<std::vector<QueryResponseData::Question>>& questions,
    boost::optional<std::vector<QueryResponseData::RR>>& answers,
    boost::optional<std::vector<QueryResponseData::RR>>& authorities,
    boost::optional<std::vector<QueryResponseData::RR>>& additionals
    )
{
    if ( !extra_info )
        return;

    if ( extra_info->questions_list )
    {
        std::vector<QueryResponseData::Question> qvec;
        for ( auto& qid : block_->questions_lists[*extra_info->questions_list].vec )
        {
            const block_cbor::Question& q = block_->questions[*qid];
            QueryResponseData::Question newq;

            if ( q.qname )
                newq.qname = block_->names_rdatas[*q.qname].str;
            else
                newq.qname = defaults_.query_name;

            if ( q.classtype )
            {
                const block_cbor::ClassType& ct = block_->class_types[*q.classtype];
                newq.qclass = ct.qclass;
                newq.qtype = ct.qtype;
            }
            else
            {
                newq.qclass = defaults_.query_class;
                newq.qtype = defaults_.query_type;
            }
            qvec.push_back(newq);
        }
        questions = qvec;
    }

    if ( extra_info->answers_list )
        read_rr(extra_info->answers_list, answers);
    if ( extra_info->authority_list )
        read_rr(extra_info->authority_list, authorities);
    if ( extra_info->additional_list )
        read_rr(extra_info->additional_list, additionals);
}

void BlockCborReader::read_rr(block_cbor::index_t index, boost::optional<std::vector<QueryResponseData::RR>>& res)
{
    if ( index )
    {
        std::vector<QueryResponseData::RR> rrvec;
        for ( auto& rrid : block_->rrs_lists[*index].vec )
        {
            const block_cbor::ResourceRecord& rr = block_->resource_records[*rrid];
            QueryResponseData::RR newrr;

            if ( rr.name )
                newrr.name = block_->names_rdatas[*rr.name].str;
            else
                newrr.name = defaults_.query_name;

            if ( rr.classtype )
            {
                const block_cbor::ClassType& ct = block_->class_types[*rr.classtype];
                newrr.rclass = ct.qclass;
                newrr.rtype = ct.qtype;
            }
            else
            {
                newrr.rclass = defaults_.query_class;
                newrr.rtype = defaults_.query_type;
            }

            newrr.ttl = rr.ttl;

            if ( rr.rdata )
                newrr.rdata = block_->names_rdatas[*rr.rdata].str;
            else
                newrr.rdata = defaults_.rr_rdata;

            rrvec.push_back(newrr);
        }
        res = rrvec;
    }
}

std::shared_ptr<QueryResponse> BlockCborReader::readQR()
{
    std::shared_ptr<QueryResponse> res;
    std::unique_ptr<DNSMessage> query, response;

    while ( need_block_ )
        if ( !readBlock() )
            return res;

    const block_cbor::QueryResponseItem& qri = block_->query_response_items[next_item_];
    need_block_ = (block_->query_response_items.size() == ++next_item_);

    const block_cbor::QueryResponseSignature& sig = block_->query_response_signatures[qri.signature];
    uint16_t dns_flags;
    uint8_t transport_flags = block_cbor::convert_transport_flags(*sig.qr_transport_flags, file_format_version_);
    bool ipv6 = (transport_flags & block_cbor::IPV6);

    if ( sig.qr_flags & block_cbor::QUERY_ONLY )
    {
        query = make_unique<DNSMessage>();
        query->timestamp = *qri.tstamp;
        query->tcp = transport_flags & block_cbor::TCP;
        query->clientIP = string_to_addr(block_->ip_addresses[*qri.client_address].str, ipv6);
        query->serverIP = string_to_addr(block_->ip_addresses[*sig.server_address].str, ipv6);
        query->clientPort = *qri.client_port;
        query->serverPort = *sig.server_port;
        query->hoplimit = *qri.hoplimit;
        query->dns.type(CaptureDNS::QRType::QUERY);
        query->dns.id(*qri.id);
        query->dns.opcode(*sig.query_opcode);
        query->dns.rcode(*sig.query_rcode);
        query->wire_size = *qri.query_size;

        dns_flags = block_cbor::convert_dns_flags(*sig.dns_flags, file_format_version_);
        block_cbor::set_dns_flags(*query, dns_flags, true);

        if ( qri.query_extra_info )
            readExtraInfo(*query, *(qri.query_extra_info));

        if ( sig.qr_flags & block_cbor::QUERY_HAS_OPT )
        {
            byte_string opt_rdata = block_->names_rdatas[*sig.query_opt_rdata].str;
#if ENABLE_PSEUDOANONYMISATION
            if ( pseudo_anon_ )
            {
                CaptureDNS::EDNS0 edns0(CaptureDNS::INTERNET,
                                        0,
                                        opt_rdata);
                edns0 = pseudo_anon_->edns0(edns0);
                opt_rdata = edns0.rr().data();
            }
#endif

            uint32_t ttl = ((*sig.query_rcode >> 4) &0xff);
            ttl <<= 8;
            ttl |= (*sig.query_edns_version & 0xff);
            ttl <<= 16;
            if ( *sig.dns_flags & block_cbor::QUERY_DO )
                ttl |= 0x8000;
            query->dns.add_additional(
                CaptureDNS::resource(
                    "",
                    opt_rdata,
                    CaptureDNS::OPT,
                    static_cast<CaptureDNS::QueryClass>(*sig.query_edns_payload_size),
                    ttl));
        }
    }

    if ( sig.qr_flags & block_cbor::RESPONSE_ONLY )
    {
        response = make_unique<DNSMessage>();
        response->timestamp = *qri.tstamp + *qri.response_delay;
        response->clientIP = string_to_addr(block_->ip_addresses[*qri.client_address].str, ipv6);
        response->serverIP = string_to_addr(block_->ip_addresses[*sig.server_address].str, ipv6);
        response->clientPort = *qri.client_port;
        response->serverPort = *sig.server_port;
        response->tcp = transport_flags & block_cbor::TCP;
        response->dns.type(CaptureDNS::QRType::RESPONSE);
        response->dns.id(*qri.id);
        response->dns.opcode(*sig.query_opcode);
        response->dns.rcode(*sig.response_rcode);
        response->wire_size = *qri.response_size;

        dns_flags = block_cbor::convert_dns_flags(*sig.dns_flags, file_format_version_);
        block_cbor::set_dns_flags(*response, *sig.dns_flags, false);

        if ( qri.response_extra_info )
            readExtraInfo(*response, *(qri.response_extra_info));
    }

    if ( sig.qr_flags & block_cbor::QR_HAS_QUESTION )
    {
        CaptureDNS::query q = makeQuery(*qri.qname, *sig.query_classtype);
        if ( query )
            query->dns.add_query(q);
        if ( response && !( sig.qr_flags & block_cbor::RESPONSE_HAS_NO_QUESTION ) ) {
            response->dns.add_query(q);
        }
    }

    if ( query )
    {
        res = std::make_shared<QueryResponse>(std::move(query));
        if ( response )
            res->set_response(std::move(response));
    }
    else
    {
        res = std::make_shared<QueryResponse>(std::move(response), false);
    }

    return res;
}

void BlockCborReader::readExtraInfo(DNSMessage& dns, const block_cbor::QueryResponseExtraInfo& extra) const
{
    if ( extra.questions_list )
        for ( auto& q_id : block_->questions_lists[*extra.questions_list].vec )
        {
            const block_cbor::Question& q = block_->questions[*q_id];
            dns.dns.add_query(makeQuery(q.qname, q.classtype));
        }

    if ( extra.answers_list )
        for ( auto& rr_id : block_->rrs_lists[*extra.answers_list].vec )
            dns.dns.add_answer(makeResource(block_->resource_records[*rr_id]));

    if ( extra.authority_list )
        for ( auto& rr_id : block_->rrs_lists[*extra.authority_list].vec )
            dns.dns.add_authority(makeResource(block_->resource_records[*rr_id]));

    if ( extra.additional_list )
        for ( auto& rr_id : block_->rrs_lists[*extra.additional_list].vec )
            dns.dns.add_additional(makeResource(block_->resource_records[*rr_id]));
}

CaptureDNS::query BlockCborReader::makeQuery(block_cbor::index_t qname_id, block_cbor::index_t class_type_id) const
{
    byte_string qname = block_->names_rdatas[*qname_id].str;
    const block_cbor::ClassType& ct = block_->class_types[*class_type_id];
    return CaptureDNS::query(qname,
                            static_cast<CaptureDNS::QueryType>(*ct.qtype),
                            static_cast<CaptureDNS::QueryClass>(*ct.qclass));
}

CaptureDNS::resource BlockCborReader::makeResource(const block_cbor::ResourceRecord& rr) const
{
    byte_string name = block_->names_rdatas[*rr.name].str;
    const block_cbor::ClassType& ct = block_->class_types[*rr.classtype];
    byte_string rdata = block_->names_rdatas[*rr.rdata].str;

    CaptureDNS::resource res(name,
                             rdata,
                             static_cast<CaptureDNS::QueryType>(*ct.qtype),
                             static_cast<CaptureDNS::QueryClass>(*ct.qclass),
                             *rr.ttl);

#if ENABLE_PSEUDOANONYMISATION
    if ( ct.qtype == CaptureDNS::OPT && pseudo_anon_ )
    {
        CaptureDNS::EDNS0 edns0(res);
        edns0 = pseudo_anon_->edns0(edns0);
        res = edns0.rr();
    }
#endif

    return res;
}

IPAddress BlockCborReader::string_to_addr(const byte_string& str, bool is_ipv6)
{
    IPAddress res;
    byte_string b = str;

    // Storing transport flags is optional if full addresses are stored.
    // If the address is more than 4 bytes long, it's definitely IPv6,
    // whatever is_ipv6 may think.
    if ( is_ipv6 || str.size() > 4 )
    {
        if ( str.size() < 16 )
            b.resize(16, 0);
    }
    else
    {
        if ( str.size() < 4 )
            b.resize(4, 0);
    }

    res = IPAddress(b);

#if ENABLE_PSEUDOANONYMISATION
    if ( pseudo_anon_ )
        res = pseudo_anon_->address(res);
#endif

    return res;
}

bool BlockCborReader::is_ipv4_client_full_address(const byte_string& b) const
{
    const block_cbor::BlockParameters& bp = block_parameters_[block_->block_parameters_index];
    const block_cbor::StorageParameters& sp = bp.storage_parameters;

    return ( sp.client_address_prefix_ipv4 == 32 && b.length() == 4 );
}

bool BlockCborReader::is_ipv6_client_full_address(const byte_string& b) const
{
    const block_cbor::BlockParameters& bp = block_parameters_[block_->block_parameters_index];
    const block_cbor::StorageParameters& sp = bp.storage_parameters;

    return ( sp.client_address_prefix_ipv4 == 128 && b.length() == 16 );
}

IPAddress BlockCborReader::get_client_address(std::size_t index, boost::optional<uint8_t> transport_flags)
{
    bool ipv6;
    const byte_string& addr_b = block_->ip_addresses[index].str;

    if ( is_ipv4_client_full_address(addr_b) )
        ipv6 = false;
    else if ( is_ipv6_client_full_address(addr_b) )
        ipv6 = true;
    else
        ipv6 = (*transport_flags & block_cbor::IPV6);

    return string_to_addr(addr_b, ipv6);
}

bool BlockCborReader::is_ipv4_server_full_address(const byte_string& b) const
{
    const block_cbor::BlockParameters& bp = block_parameters_[block_->block_parameters_index];
    const block_cbor::StorageParameters& sp = bp.storage_parameters;

    return ( sp.server_address_prefix_ipv4 == 32 && b.length() == 4 );
}

bool BlockCborReader::is_ipv6_server_full_address(const byte_string& b) const
{
    const block_cbor::BlockParameters& bp = block_parameters_[block_->block_parameters_index];
    const block_cbor::StorageParameters& sp = bp.storage_parameters;

    return ( sp.server_address_prefix_ipv4 == 128 && b.length() == 16 );
}

IPAddress BlockCborReader::get_server_address(std::size_t index, boost::optional<uint8_t> transport_flags)
{
    bool ipv6;
    const byte_string& addr_b = block_->ip_addresses[index].str;

    if ( is_ipv4_server_full_address(addr_b) )
        ipv6 = false;
    else if ( is_ipv6_server_full_address(addr_b) )
        ipv6 = true;
    else
        ipv6 = (*transport_flags & block_cbor::IPV6);

    return string_to_addr(addr_b, ipv6);
}

void BlockCborReader::dump_collector(std::ostream& os)
{
    os << "\nCOLLECTOR:"
       << "\n  Collector ID         : " << generator_id_
       << "\n  Collection host ID   : " << host_id_
       << "\n";
}

void BlockCborReader::dump_address_events(std::ostream& os)
{
    for ( unsigned event_type = AddressEvent::EventType::TCP_RESET;
          event_type <= AddressEvent::EventType::ICMPv6_PACKET_TOO_BIG;
          ++event_type )
    {
        bool ignore_code = false, seen_one = false;
        std::string title;
        switch (event_type)
        {
        case AddressEvent::EventType::TCP_RESET:
            title = "TCP RESETS";
            ignore_code = true;
            break;

        case AddressEvent::EventType::ICMP_TIME_EXCEEDED:
            title = "ICMP TIME EXCEEDED";
            break;

        case AddressEvent::EventType::ICMP_DEST_UNREACHABLE:
            title = "ICMP DEST UNREACHABLE";
            break;

        case AddressEvent::EventType::ICMPv6_TIME_EXCEEDED:
            title = "ICMPv6 TIME EXCEEDED";
            break;

        case AddressEvent::EventType::ICMPv6_DEST_UNREACHABLE:
            title = "ICMPv6 DEST UNREACHABLE";
            break;

        case AddressEvent::EventType::ICMPv6_PACKET_TOO_BIG:
            title = "ICMPv6 PACKET TOO BIG";
            break;
        }

        struct AEInfo
        {
            AEInfo(const AddressEvent& ae, unsigned count)
                : ae_(ae), count_(count) {}

            bool operator<(const AEInfo& rhs) const
            {
                if ( ae_.code() < rhs.ae_.code() )
                    return true;
                else if ( ae_.code() == rhs.ae_.code() )
                {
                    if ( count_ < rhs.count_ )
                        return true;
                    else if ( count_ == rhs.count_ )
                        return ( ae_.address() < rhs.ae_.address() );
                }
                return false;
            }

            AddressEvent ae_;
            unsigned count_;
        };

        std::vector<AEInfo> aeinfo;

        for ( auto& ae : address_events_read_ )
        {
            if ( ae.first.type() == event_type )
                aeinfo.emplace_back(ae.first, ae.second);
        }

        std::sort(aeinfo.begin(), aeinfo.end());

        for ( auto& aei : aeinfo )
        {
            if ( !seen_one )
            {
                os << title << ":\n";
                seen_one = true;
            }

            if ( !ignore_code )
                os << "  Code: " << std::setw(2) << aei.ae_.code();
            os << "  Count: " << std::setw(5) << aei.count_;
            os << "  Address: " << aei.ae_.address() << "\n";
        }

        if ( seen_one )
            os << "\n";
    }
}
