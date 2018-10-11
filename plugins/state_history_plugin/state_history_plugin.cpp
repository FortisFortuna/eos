/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <eosio/state_history_plugin/history_log.hpp>
#include <eosio/state_history_plugin/state_history_plugin.hpp>
#include <eosio/state_history_plugin/state_history_serialization.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/signals2/connection.hpp>

using tcp    = boost::asio::ip::tcp;
namespace ws = boost::beast::websocket;

extern const char* const state_history_plugin_abi;

namespace eosio {
using namespace chain;
using boost::signals2::scoped_connection;

static appbase::abstract_plugin& _state_history_plugin = app().register_plugin<state_history_plugin>();

template <typename F>
auto catch_and_log(F f) {
   try {
      return f();
   } catch (const fc::exception& e) {
      elog("${e}", ("e", e.to_detail_string()));
   } catch (const std::exception& e) {
      elog("${e}", ("e", e.what()));
   } catch (...) {
      elog("unknown exception");
   }
}

struct state_history_plugin_impl : std::enable_shared_from_this<state_history_plugin_impl> {
   chain_plugin*                                        chain_plug = nullptr;
   history_log                                          block_state_log{"block_state_history"};
   history_log                                          trace_log{"trace_history"};
   history_log                                          chain_state_log{"chain_state_history"};
   bool                                                 stopping = false;
   fc::optional<scoped_connection>                      applied_transaction_connection;
   fc::optional<scoped_connection>                      accepted_block_connection;
   string                                               endpoint_address = "0.0.0.0";
   uint16_t                                             endpoint_port    = 4321;
   std::unique_ptr<tcp::acceptor>                       acceptor;
   std::map<transaction_id_type, transaction_trace_ptr> traces;

   void get_data(history_log& log, uint32_t block_num, fc::optional<bytes>& result) {
      if (block_num < log.begin_block() || block_num >= log.end_block())
         return;
      history_log_header header;
      auto&              stream = log.get_entry(block_num, header);
      uint32_t           s;
      stream.read((char*)&s, sizeof(s));
      result.emplace();
      result->resize(s);
      if (s)
         stream.read(result->data(), s);
   }

   void get_block(uint32_t block_num, fc::optional<bytes>& result) {
      auto p = chain_plug->chain().fetch_block_by_number(block_num);
      result = fc::raw::pack(*p);
   }

   struct session : std::enable_shared_from_this<session> {
      std::shared_ptr<state_history_plugin_impl> plugin;
      std::unique_ptr<ws::stream<tcp::socket>>   stream;
      bool                                       sending = false;
      bool                                       sentAbi = false;
      std::vector<std::vector<char>>             send_queue;

      session(std::shared_ptr<state_history_plugin_impl> plugin)
          : plugin(std::move(plugin)) {}

      void start(tcp::socket socket) {
         ilog("incoming connection");
         stream = std::make_unique<ws::stream<tcp::socket>>(std::move(socket));
         stream->binary(true);
         stream->next_layer().set_option(boost::asio::ip::tcp::no_delay(true));
         stream->next_layer().set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024));
         stream->next_layer().set_option(boost::asio::socket_base::receive_buffer_size(1024 * 1024));
         stream->async_accept([self = shared_from_this(), this](boost::system::error_code ec) {
            if (plugin->stopping)
               return;
            callback(ec, "async_accept", [&] {
               start_read();
               send(state_history_plugin_abi);
            });
         });
      }

      void start_read() {
         auto in_buffer = std::make_shared<boost::beast::flat_buffer>();
         stream->async_read(
             *in_buffer, [self = shared_from_this(), this, in_buffer](boost::system::error_code ec, size_t) {
                if (plugin->stopping)
                   return;
                callback(ec, "async_read", [&] {
                   auto d = boost::asio::buffer_cast<char const*>(boost::beast::buffers_front(in_buffer->data()));
                   auto s = boost::asio::buffer_size(in_buffer->data());
                   fc::datastream<const char*> ds(d, s);
                   state_request               req;
                   fc::raw::unpack(ds, req);
                   req.visit(*this);
                   start_read();
                });
             });
      }

      void send(const char* s) {
         send_queue.push_back({s, s + strlen(s)});
         send();
      }

      template <typename T>
      void send(T obj) {
         send_queue.push_back(fc::raw::pack(state_result{std::move(obj)}));
         send();
      }

      void send() {
         if (sending || send_queue.empty())
            return;
         sending = true;
         stream->binary(sentAbi);
         sentAbi = true;
         stream->async_write( //
             boost::asio::buffer(send_queue[0]),
             [self = shared_from_this(), this](boost::system::error_code ec, size_t) {
                if (plugin->stopping)
                   return;
                callback(ec, "async_write", [&] {
                   send_queue.erase(send_queue.begin());
                   sending = false;
                   send();
                });
             });
      }

      using result_type = void;
      void operator()(get_status_request_v0&) {
         auto&                chain = plugin->chain_plug->chain();
         get_status_result_v0 result;
         result.last_irreversible_block_num = chain.last_irreversible_block_num();
         result.last_irreversible_block_id  = chain.last_irreversible_block_id();
         result.state_begin_block_num       = plugin->chain_state_log.begin_block();
         result.state_end_block_num         = plugin->chain_state_log.end_block();
         send(std::move(result));
      }

      void operator()(get_block_request_v0& req) {
         // ilog("${b} get_block_request_v0", ("b", req.block_num));
         get_block_result_v0 result{req.block_num};
         // todo: client select which datasets to receive
         plugin->get_block(req.block_num, result.block);
         plugin->get_data(plugin->block_state_log, req.block_num, result.block_state);
         plugin->get_data(plugin->trace_log, req.block_num, result.traces);
         plugin->get_data(plugin->chain_state_log, req.block_num, result.deltas);
         send(std::move(result));
      }

      template <typename F>
      void catch_and_close(F f) {
         try {
            f();
         } catch (const fc::exception& e) {
            elog("${e}", ("e", e.to_detail_string()));
            close();
         } catch (const std::exception& e) {
            elog("${e}", ("e", e.what()));
            close();
         } catch (...) {
            elog("unknown exception");
            close();
         }
      }

      template <typename F>
      void callback(boost::system::error_code ec, const char* what, F f) {
         if (ec)
            return on_fail(ec, what);
         catch_and_close(f);
      }

      void on_fail(boost::system::error_code ec, const char* what) {
         try {
            elog("${w}: ${m}", ("w", what)("m", ec.message()));
            close();
         } catch (...) {
            elog("uncaught exception on close");
         }
      }

      void close() {
         stream->next_layer().close();
         plugin->sessions.erase(this);
      }
   };
   std::map<session*, std::shared_ptr<session>> sessions;

   void listen() {
      boost::system::error_code ec;
      auto                      address  = boost::asio::ip::make_address(endpoint_address);
      auto                      endpoint = tcp::endpoint{address, endpoint_port};
      acceptor                           = std::make_unique<tcp::acceptor>(app().get_io_service());

      auto check_ec = [&](const char* what) {
         if (!ec)
            return;
         elog("${w}: ${m}", ("w", what)("m", ec.message()));
         EOS_ASSERT(false, plugin_exception, "unable top open listen socket");
      };

      acceptor->open(endpoint.protocol(), ec);
      check_ec("open");
      acceptor->set_option(boost::asio::socket_base::reuse_address(true));
      acceptor->bind(endpoint, ec);
      check_ec("bind");
      acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
      check_ec("listen");
      do_accept();
   }

   void do_accept() {
      auto socket = std::make_shared<tcp::socket>(app().get_io_service());
      acceptor->async_accept(*socket, [self = shared_from_this(), socket, this](auto ec) {
         if (stopping)
            return;
         if (ec) {
            if (ec == boost::system::errc::too_many_files_open)
               catch_and_log([&] { do_accept(); });
            return;
         }
         catch_and_log([&] {
            auto s            = std::make_shared<session>(self);
            sessions[s.get()] = s;
            s->start(std::move(*socket));
         });
         do_accept();
      });
   }

   void on_applied_transaction(const transaction_trace_ptr& p) {
      if (p->receipt)
         traces[p->id] = p;
   }

   void on_accepted_block(const block_state_ptr& block_state) {
      // todo: config options
      store_block_state(block_state);
      store_traces(block_state);
      store_chain_state(block_state);
   }

   void store_block_state(const block_state_ptr& block_state) {
      // todo
   }

   void store_traces(const block_state_ptr& block_state) {
      std::vector<transaction_trace_ptr> traces;
      for (auto& p : block_state->trxs) {
         auto it = this->traces.find(p->id);
         if (it == this->traces.end() || !it->second->receipt) {
            ilog("missing trace for transaction {id}", ("id", p->id));
            continue;
         }
         traces.push_back(it->second);
      }
      this->traces.clear();

      auto traces_bin = fc::raw::pack(make_history_serial_wrapper(traces));
      EOS_ASSERT(traces_bin.size() == (uint32_t)traces_bin.size(), plugin_exception, "traces is too big");

      history_log_header header{.block_num    = block_state->block->block_num(),
                                .block_id     = block_state->block->id(),
                                .payload_size = sizeof(uint32_t) + traces_bin.size()};
      trace_log.write_entry(header, block_state->block->previous, [&](auto& stream) {
         uint32_t s = (uint32_t)traces_bin.size();
         stream.write((char*)&s, sizeof(s));
         if (!traces_bin.empty())
            stream.write(traces_bin.data(), traces_bin.size());
      });
   }

   void store_chain_state(const block_state_ptr& block_state) {
      bool fresh = chain_state_log.begin_block() == chain_state_log.end_block();
      if (fresh)
         ilog("Placing initial state in block ${n}", ("n", block_state->block->block_num()));

      std::vector<table_delta> deltas;
      auto&                    db = chain_plug->chain().db();

      const auto&                                table_id_index = db.get_index<table_id_multi_index>();
      std::map<uint64_t, const table_id_object*> removed_table_id;
      for (auto& rem : table_id_index.stack().back().removed_values)
         removed_table_id[rem.first._id] = &rem.second;

      auto get_table_id = [&](uint64_t tid) -> const table_id_object& {
         auto obj = table_id_index.find(tid);
         if (obj)
            return *obj;
         auto it = removed_table_id.find(tid);
         EOS_ASSERT(it != removed_table_id.end(), chain::plugin_exception, "can not found table id ${tid}",
                    ("tid", tid));
         return *it->second;
      };

      auto pack_row          = [](auto& row) { return fc::raw::pack(make_history_serial_wrapper(row)); };
      auto pack_contract_row = [&](auto& row) {
         return fc::raw::pack(make_history_table_wrapper(get_table_id(row.t_id._id), row));
      };

      auto process_table = [&](auto* name, auto& index, auto& pack_row) {
         if (fresh) {
            if (index.indices().empty())
               return;
            deltas.push_back({});
            auto& delta = deltas.back();
            delta.name  = name;
            for (auto& row : index.indices())
               delta.rows.emplace_back(row.id._id, pack_row(row));
         } else {
            if (index.stack().empty())
               return;
            auto& undo = index.stack().back();
            if (undo.old_values.empty() && undo.new_ids.empty() && undo.removed_values.empty())
               return;
            deltas.push_back({});
            auto& delta = deltas.back();
            delta.name  = name;
            for (auto& old : undo.old_values) {
               auto& row = index.get(old.first);
               delta.rows.emplace_back(true, pack_row(row));
            }
            for (auto id : undo.new_ids) {
               auto& row = index.get(id);
               delta.rows.emplace_back(true, pack_row(row));
            }
            for (auto& old : undo.removed_values)
               delta.rows.emplace_back(false, pack_row(old.second));
         }
      };

      process_table("account", db.get_index<account_index>(), pack_row);

      process_table("contract_table", db.get_index<table_id_multi_index>(), pack_row);
      process_table("contract_row", db.get_index<key_value_index>(), pack_contract_row);
      process_table("contract_index64", db.get_index<index64_index>(), pack_contract_row);
      process_table("contract_index128", db.get_index<index128_index>(), pack_contract_row);
      process_table("contract_index256", db.get_index<index256_index>(), pack_contract_row);
      process_table("contract_index_double", db.get_index<index_double_index>(), pack_contract_row);
      process_table("contract_index_long_double", db.get_index<index_long_double_index>(), pack_contract_row);

      process_table("global_property", db.get_index<global_property_multi_index>(), pack_row);
      process_table("generated_transaction", db.get_index<generated_transaction_multi_index>(), pack_row);

      process_table("permission", db.get_index<permission_index>(), pack_row);
      process_table("permission_link", db.get_index<permission_link_index>(), pack_row);

      process_table("resource_limits", db.get_index<resource_limits::resource_limits_index>(), pack_row);
      process_table("resource_usage", db.get_index<resource_limits::resource_usage_index>(), pack_row);
      process_table("resource_limits_state", db.get_index<resource_limits::resource_limits_state_index>(), pack_row);
      process_table("resource_limits_config", db.get_index<resource_limits::resource_limits_config_index>(), pack_row);

      auto deltas_bin = fc::raw::pack(deltas);
      EOS_ASSERT(deltas_bin.size() == (uint32_t)deltas_bin.size(), plugin_exception, "deltas is too big");
      history_log_header header{.block_num    = block_state->block->block_num(),
                                .block_id     = block_state->block->id(),
                                .payload_size = sizeof(uint32_t) + deltas_bin.size()};
      chain_state_log.write_entry(header, block_state->block->previous, [&](auto& stream) {
         uint32_t s = (uint32_t)deltas_bin.size();
         stream.write((char*)&s, sizeof(s));
         if (!deltas_bin.empty())
            stream.write(deltas_bin.data(), deltas_bin.size());
      });
   } // store_chain_state
};   // state_history_plugin_impl

state_history_plugin::state_history_plugin()
    : my(std::make_shared<state_history_plugin_impl>()) {}

state_history_plugin::~state_history_plugin() {}

void state_history_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto options = cfg.add_options();
   options("state-history-dir", bpo::value<bfs::path>()->default_value("state-history"),
           "the location of the state-history directory (absolute path or relative to application data dir)");
   options("delete-state-history", bpo::bool_switch()->default_value(false), "clear state history database");
   options("state-history-endpoint", bpo::value<string>()->default_value("0.0.0.0:8080"),
           "the endpoint upon which to listen for incoming connections");
}

void state_history_plugin::plugin_initialize(const variables_map& options) {
   try {
      // todo: check for --disable-replay-opts

      my->chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT(my->chain_plug, chain::missing_chain_plugin_exception, "");
      auto& chain = my->chain_plug->chain();
      my->applied_transaction_connection.emplace(
          chain.applied_transaction.connect([&](const transaction_trace_ptr& p) { my->on_applied_transaction(p); }));
      my->accepted_block_connection.emplace(
          chain.accepted_block.connect([&](const block_state_ptr& p) { my->on_accepted_block(p); }));

      auto                    dir_option = options.at("state-history-dir").as<bfs::path>();
      boost::filesystem::path state_history_dir;
      if (dir_option.is_relative())
         state_history_dir = app().data_dir() / dir_option;
      else
         state_history_dir = dir_option;

      auto ip_port         = options.at("state-history-endpoint").as<string>();
      auto port            = ip_port.substr(ip_port.find(':') + 1, ip_port.size());
      auto host            = ip_port.substr(0, ip_port.find(':'));
      my->endpoint_address = host;
      my->endpoint_port    = std::stoi(port);
      idump((ip_port)(host)(port));

      if (options.at("delete-state-history").as<bool>()) {
         ilog("Deleting state history");
         boost::filesystem::remove_all(state_history_dir);
      }

      boost::filesystem::create_directories(state_history_dir);
      my->block_state_log.open((state_history_dir / "block_state_history.log").string(),
                               (state_history_dir / "block_state_history.index").string());
      my->trace_log.open((state_history_dir / "trace_history.log").string(),
                         (state_history_dir / "trace_history.index").string());
      my->chain_state_log.open((state_history_dir / "chain_state_history.log").string(),
                               (state_history_dir / "chain_state_history.index").string());
   }
   FC_LOG_AND_RETHROW()
} // state_history_plugin::plugin_initialize

void state_history_plugin::plugin_startup() { my->listen(); }

void state_history_plugin::plugin_shutdown() {
   my->applied_transaction_connection.reset();
   my->accepted_block_connection.reset();
   while (!my->sessions.empty())
      my->sessions.begin()->second->close();
   my->stopping = true;
}

} // namespace eosio
