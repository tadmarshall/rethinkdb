#include "server.hpp"
#include "db_thread_info.hpp"
#include "memcached/memcached.hpp"
#include "diskinfo.hpp"
#include "concurrency/cond_var.hpp"
#include "logger.hpp"
#include "server/cmd_args.hpp"
#include "replication/slave.hpp"

int run_server(int argc, char *argv[]) {

    // Parse command line arguments
    cmd_config_t config = parse_cmd_args(argc, argv);

    // Open the log file, if necessary.
    if (config.log_file_name[0]) {
        log_file = fopen(config.log_file_name, "w");
    }

    // Initial thread message to start server
    struct server_starter_t :
        public thread_message_t
    {
        cmd_config_t *cmd_config;
        thread_pool_t *thread_pool;
        void on_thread_switch() {
            coro_t::spawn(&server_main, cmd_config, thread_pool);
        }
    } starter;
    starter.cmd_config = &config;

    // Run the server.
    thread_pool_t thread_pool(config.n_workers);
    starter.thread_pool = &thread_pool;
    thread_pool.run(&starter);

    logINF("Server is shut down.\n");

    // Close the log file if necessary.
    if (config.log_file_name[0]) {
        fclose(log_file);
        log_file = stderr;
    }

    return 0;
}

void server_main(cmd_config_t *cmd_config, thread_pool_t *thread_pool) {

    /* Create a server object. It only exists so we can give pointers to it to other things. */
    server_t server(cmd_config, thread_pool);

    {
        /* Start logger */
        log_controller_t log_controller;
    
        /* Copy database filenames from private serializer configurations into a single vector of strings */
        std::vector<std::string> db_filenames;
        std::vector<log_serializer_private_dynamic_config_t>& serializer_private = cmd_config->store_dynamic_config.serializer_private;
        std::vector<log_serializer_private_dynamic_config_t>::iterator it;
    
        for (it = serializer_private.begin(); it != serializer_private.end(); ++it) {
            db_filenames.push_back((*it).db_filename);
        }
    
        /* Check to see if there is an existing database */
        struct : public btree_key_value_store_t::check_callback_t, public promise_t<bool> {
            void on_store_check(bool ok) { pulse(ok); }
        } check_cb;
        btree_key_value_store_t::check_existing(db_filenames, &check_cb);
        bool existing = check_cb.wait();
        if (existing && cmd_config->create_store && !cmd_config->force_create) {
            fail_due_to_user_error(
                "It looks like there already is a database here. RethinkDB will abort in case you "
                "didn't mean to overwrite it. Run with the '--force' flag to override this warning.");
        } else {
            if (!existing) {
                cmd_config->create_store = true;
            }
        }

        /* Record information about disk drives to log file */
        log_disk_info(cmd_config->store_dynamic_config.serializer_private);

        /* Create store if necessary */
        if (cmd_config->create_store) {
            logINF("Creating database...\n");
            btree_key_value_store_t::create(
                &cmd_config->store_dynamic_config,
                &cmd_config->store_static_config);
            logINF("Done creating.\n");
        }

        if (!cmd_config->shutdown_after_creation) {

            /* Start key-value store */
            logINF("Loading database...\n");
            btree_key_value_store_t store(&cmd_config->store_dynamic_config);

            /* Are we a replication slave? */
            if (cmd_config->replication_config.active) {
                logINF("Starting up as a slave...\n");
                replication::slave_t slave_store(&store, cmd_config->replication_config);
                server.store = &slave_store;
            } else {
                server.store = &store;   /* So things can access it */
            }

            /* Start connection acceptor */
            struct : public conn_acceptor_t::handler_t {
                server_t *parent;
                void handle(tcp_conn_t *conn) {
                    serve_memcache(conn, parent->store);
                }
            } handler;
            handler.parent = &server;

            try {
                conn_acceptor_t conn_acceptor(cmd_config->port, &handler);

                logINF("Server is now accepting memcache connections on port %d.\n", cmd_config->port);

                /* Wait for an order to shut down */
                struct : public thread_message_t, public cond_t {
                    void on_thread_switch() { pulse(); }
                } interrupt_cond;
                thread_pool_t::set_interrupt_message(&interrupt_cond);
                interrupt_cond.wait();

                logINF("Shutting down... this may take time if there is a lot of unsaved data.\n");

            } catch (conn_acceptor_t::address_in_use_exc_t) {
                logERR("Port %d is already in use -- aborting.\n", cmd_config->port);
            }

            // store destructor called here

        } else {
        
            logINF("Shutting down...\n");
        }
    }

    /* The penultimate step of shutting down is to make sure that all messages
    have reached their destinations so that they can be freed. The way we do this
    is to send one final message to each core; when those messages all get back
    we know that all messages have been processed properly. Otherwise, logger
    shutdown messages would get "stuck" in the message hub when it shut down,
    leading to memory leaks. */
    for (int i = 0; i < get_num_threads(); i++) {
        on_thread_t thread_switcher(i);
    }

    /* Finally tell the thread pool to stop. TODO: Eventually, the thread pool should stop
    automatically when server_main() returns. */
    thread_pool->shutdown();
}
