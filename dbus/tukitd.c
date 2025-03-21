#include "Bindings/libtukit.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>
#include <wordexp.h>

enum transactionstates { queued, running, finished };

typedef struct t_entry {
    char* id;
    struct t_entry *next;
    enum transactionstates state;
} TransactionEntry;

// Even though userdata / activeTransaction is shared between several threads, due to
// systemd's serial event loop processing it's always guaranteed that no parallel
// access will be happen: lockSnapshot will be called in the event functions before
// starting the new thread, and unlockSnapshot is triggered by the signal when a
// thread has finished.
// Any method which does write the variable must do so from the main event loop.
int lockSnapshot(void* userdata, const char* transaction, sd_bus_error *ret_error) {
    fprintf(stdout, "Locking further invocations for snapshot %s...\n", transaction);
    TransactionEntry* activeTransaction = userdata;
    TransactionEntry* newTransaction;
    while (activeTransaction->id != NULL) {
        if (strcmp(activeTransaction->id, transaction) == 0) {
            sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "The transaction is currently in use by another thread.");
            return -EBUSY;
        }
        activeTransaction = activeTransaction->next;
    }
    if ((newTransaction = malloc(sizeof(TransactionEntry))) == NULL) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Error while allocating space for transaction.");
        return -ENOMEM;
    }
    newTransaction->id = NULL;
    activeTransaction->id = strdup(transaction);
    if (activeTransaction->id == NULL) {
        free(newTransaction);
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Error during strdup.");
        return -ENOMEM;
    }
    activeTransaction->state = queued;
    activeTransaction->next = newTransaction;
    return 0;
}

void unlockSnapshot(void* userdata, const char* transaction) {
    TransactionEntry* activeTransaction = userdata;
    TransactionEntry* prevNext = NULL;

    while (activeTransaction->id != NULL) {
        // The entry point can't be changed, so just set the data of the to be deleted
        // transaction to the next entry's data (also for later matches to have just one code
        // path).
        if (strcmp(activeTransaction->id, transaction) == 0) {
            fprintf(stdout, "Unlocking snapshot %s...\n", transaction);
            free(activeTransaction->id);
            activeTransaction->id = activeTransaction->next->id;
            prevNext = activeTransaction->next;
            if (activeTransaction->id != NULL) {
                activeTransaction->next = activeTransaction->next->next;
            }
            free(prevNext);
            return;
        }
        activeTransaction = activeTransaction->next;
    }
}

sd_bus* get_bus() {
    sd_bus *bus = NULL;
    if (sd_bus_default_system(&bus) < 0) {
        // When opening a new bus connection fails there aren't a lot of options to present this
        // error to the user here. Maybe add some synchronization with the main thread and exit
        // on error in the future?
        fprintf(stderr, "Failed to connect to system bus.");
    }
    return bus;
}

int send_error_signal(sd_bus *bus, const char *transaction, const char *message, int error) {
    if (bus == NULL) {
        bus = get_bus();
    }
    int ret = sd_bus_emit_signal(bus, "/org/opensuse/tukit", "org.opensuse.tukit", "Error", "ssi", transaction, message, error);
    if (ret < 0) {
        // Something is seriously broken when even an error message can't be sent any more...
        fprintf(stderr, "Cannot reach D-Bus any more: %s\n", strerror(ret));
    }
    sd_bus_flush_close_unref(bus);
    return ret;
}

static int transaction_open(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    char *base;
    const char *snapid;
    int ret = 0;

    if (sd_bus_message_read(m, "s", &base) < 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Could not read base snapshot identifier.");
        return -1;
    }
    struct tukit_tx* tx = tukit_new_tx();
    if (tx == NULL) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
        return -1;
    }
    if ((ret = tukit_tx_init(tx, base)) != 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
    }
    if (!ret) {
        snapid = tukit_tx_get_snapshot(tx);
        if (snapid == NULL) {
            sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
            ret = -1;
        }
    }
    if (!ret && (ret = tukit_tx_keep(tx)) != 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
    }

    tukit_free_tx(tx);
    if (ret) {
        return ret;
    }

    if (sd_bus_emit_signal(sd_bus_message_get_bus(m), "/org/opensuse/tukit", "org.opensuse.tukit.Transaction", "TransactionOpened", "s", snapid) < 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Sending signal 'TransactionOpened' failed.");
        return -1;
    }

    fprintf(stdout, "Snapshot %s created.\n", snapid);

    ret = sd_bus_reply_method_return(m, "s", snapid);
    free((void*)snapid);
    return ret;
}

struct execution_args {
    char *transaction;
    char *command;
    int chrooted;
    enum transactionstates *state;
};

static void *execution_func(void *args) {
    int ret = 0;
    int exec_ret = 0;
    wordexp_t p;

    struct execution_args* ea = (struct execution_args*)args;
    char transaction[strlen(ea->transaction) + 1]; // SIGSEGV
    strcpy(transaction, ea->transaction);
    char command[strlen(ea->command) + 1];
    strcpy(command, ea->command);
    int chrooted = ea->chrooted;

    enum transactionstates *state = ea->state;
    *state = running;

    fprintf(stdout, "Executing command `%s` in snapshot %s...\n", command, transaction);

    // The bus connection has been allocated in a parent process and is being now reused in the
    // child process, so a new dbus connection has to be established (D-Bus doesn't support
    // connection sharing between several threads). The bus will only be initialized directly
    // before it is used to avoid timeouts.
    sd_bus *bus = NULL;

    struct tukit_tx* tx = tukit_new_tx();
    if (tx == NULL) {
        send_error_signal(bus, transaction, tukit_get_errmsg(), -1);
        goto finish_execute;
    }
    ret = tukit_tx_resume(tx, transaction);
    if (ret != 0) {
        send_error_signal(bus, transaction, tukit_get_errmsg(), ret);
        goto finish_execute;
    }

    ret = wordexp(command, &p, 0);
    if (ret != 0) {
        if (ret == WRDE_NOSPACE) {
            wordfree(&p);
        }
        send_error_signal(bus, transaction, "Command could not be processed.", ret);
        goto finish_execute;
    }

    const char* output;
    if (chrooted) {
        exec_ret = tukit_tx_execute(tx, p.we_wordv, &output);
    } else {
        exec_ret = tukit_tx_call_ext(tx, p.we_wordv, &output);
    }

    wordfree(&p);

    ret = tukit_tx_keep(tx);
    if (ret != 0) {
        free((void*)output);
        send_error_signal(bus, transaction, tukit_get_errmsg(), -1);
        goto finish_execute;
    }

    bus = get_bus();
    ret = sd_bus_emit_signal(bus, "/org/opensuse/tukit", "org.opensuse.tukit.Transaction", "CommandExecuted", "sis", transaction, exec_ret, output);
    if (ret < 0) {
        send_error_signal(bus, transaction, "Cannot send signal 'CommandExecuted'.", ret);
    }

    free((void*)output);

finish_execute:
    sd_bus_flush_close_unref(bus);
    tukit_free_tx(tx);

    return (void*)(intptr_t) ret;
}

static int execute(sd_bus_message *m, void *userdata,
           sd_bus_error *ret_error, const int chrooted) {
    int ret;
    pthread_t execute_thread;
    struct execution_args exec_args;
    TransactionEntry* activeTransaction = userdata;

    if (sd_bus_message_read(m, "ss", &exec_args.transaction, &exec_args.command) < 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Could not read D-Bus parameters.");
        return -1;
    }

    ret = lockSnapshot(userdata, exec_args.transaction, ret_error);
    if (ret != 0) {
        return ret;
    }

    while (activeTransaction->next->id != NULL) {
        activeTransaction = activeTransaction->next;
    }
    exec_args.chrooted = chrooted;
    exec_args.state = &activeTransaction->state;

    ret = pthread_create(&execute_thread, NULL, execution_func, &exec_args);
    while (activeTransaction->state != running) {
        usleep(500);
    }

    pthread_detach(execute_thread);

    return ret;
}

static int transaction_call(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int ret = execute(m, userdata, ret_error, 1);
    if (ret)
        return ret;
    return sd_bus_reply_method_return(m, "");
}

static int transaction_callext(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int ret = execute(m, userdata, ret_error, 0);
    if (ret)
        return ret;
    return sd_bus_reply_method_return(m, "");
}

static int signalCallback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    char *transaction;

    if (sd_bus_message_read(m, "s", &transaction) < 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Could not read transaction ID.");
        return -1;
    }

    unlockSnapshot(userdata, transaction);
    return 0;
}

static int transaction_close(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    char *transaction;
    int ret = 0;

    if (sd_bus_message_read(m, "s", &transaction) < 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Could not read D-Bus parameters.");
        return -1;
    }
    ret = lockSnapshot(userdata, transaction, ret_error);
    if (ret != 0) {
        return ret;
    }
    struct tukit_tx* tx = tukit_new_tx();
    if (tx == NULL) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
        goto finish_close;
    }
    if ((ret = tukit_tx_resume(tx, transaction)) != 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
        goto finish_close;
    }
    if ((ret = tukit_tx_finalize(tx)) != 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
        goto finish_close;
    }

    fprintf(stdout, "Snapshot %s closed.\n", transaction);
    sd_bus_reply_method_return(m, "i", ret);

finish_close:
    tukit_free_tx(tx);
    unlockSnapshot(userdata, transaction);

    return ret;
}

static int transaction_abort(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    char *transaction;
    int ret = 0;

    if (sd_bus_message_read(m, "s", &transaction) < 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", "Could not read D-Bus parameters.");
        return -1;
    }
    ret = lockSnapshot(userdata, transaction, ret_error);
    if (ret != 0) {
        return ret;
    }
    struct tukit_tx* tx = tukit_new_tx();
    if (tx == NULL) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
	goto finish_abort;
    }
    if ((ret = tukit_tx_resume(tx, transaction)) != 0) {
        sd_bus_error_set_const(ret_error, "org.opensuse.tukit.Error", tukit_get_errmsg());
        goto finish_abort;
    }
    fprintf(stdout, "Snapshot %s aborted.\n", transaction);
    sd_bus_reply_method_return(m, "i", ret);

finish_abort:
    tukit_free_tx(tx);
    unlockSnapshot(userdata, transaction);

    return ret;
}

int event_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
    TransactionEntry* activeTransaction = userdata;
    if (activeTransaction->id != NULL) {
        fprintf(stdout, "Waiting for remaining transactions to finish...\n");
        sleep(1);
        kill(si->ssi_pid, si->ssi_signo);
        // TODO: New requests should probably be rejected from here, but unlocking is an event itself...
    } else {
        fprintf(stdout, "Terminating.\n");
        int ret;
        if ((ret = sd_event_exit(sd_event_source_get_event(s), 0)) < 0) {
            fprintf(stderr, "Cannot exit the main loop! %s\n", strerror(-ret));
            exit(1);
        }
    }
    return 0;
}

static const sd_bus_vtable tukit_transaction_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_ARGS("Open", SD_BUS_ARGS("s", base), SD_BUS_RESULT("s", snapshot), transaction_open, 0),
    SD_BUS_METHOD_WITH_ARGS("Call", SD_BUS_ARGS("s", transaction, "s", command), SD_BUS_NO_RESULT, transaction_call, 0),
    SD_BUS_METHOD_WITH_ARGS("CallExt", SD_BUS_ARGS("s", transaction, "s", command), SD_BUS_NO_RESULT, transaction_callext, 0),
    SD_BUS_METHOD_WITH_ARGS("Close", SD_BUS_ARGS("s", transaction), SD_BUS_RESULT("i", ret), transaction_close, 0),
    SD_BUS_METHOD_WITH_ARGS("Abort", SD_BUS_ARGS("s", transaction), SD_BUS_RESULT("i", ret), transaction_abort, 0),
    SD_BUS_SIGNAL_WITH_ARGS("TransactionOpened", SD_BUS_ARGS("s", snapshot), 0),
    SD_BUS_SIGNAL_WITH_ARGS("CommandExecuted", SD_BUS_ARGS("s", snapshot, "i", returncode, "s", output), 0),
    SD_BUS_VTABLE_END
};

int main() {
    fprintf(stdout, "Started tukitd %s\n", VERSION);

    sd_bus_slot *slot = NULL;
    sd_bus *bus = NULL;
    sd_event *event = NULL;
    int ret = 1;

    TransactionEntry* activeTransactions = (TransactionEntry*) malloc(sizeof(TransactionEntry));
    if (activeTransactions == NULL) {
        fprintf(stderr, "malloc failed for TransactionEntry.\n");
        goto finish;
    }
    activeTransactions->id = NULL;
    activeTransactions->state = queued;

    ret = sd_bus_open_system(&bus);
    if (ret < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-ret));
        goto finish;
    }

    ret = sd_bus_add_object_vtable(bus,
                                   &slot,
                                   "/org/opensuse/tukit/Transaction",
                                   "org.opensuse.tukit.Transaction",
                                   tukit_transaction_vtable,
                                   activeTransactions);
    if (ret < 0) {
        fprintf(stderr, "Failed to issue method call: %s\n", strerror(-ret));
        goto finish;
    }

    /* Take a well-known service name so that clients can find us */
    ret = sd_bus_request_name(bus, "org.opensuse.tukit", 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-ret));
        goto finish;
    }

    ret = sd_bus_match_signal(bus,
                NULL,
                NULL,
                "/org/opensuse/tukit",
                NULL,
                NULL,
                signalCallback,
                activeTransactions);
    if (ret < 0) {
        fprintf(stderr, "Failed to register DBus signal listener: %s\n", strerror(-ret));
        goto finish;
    }

    ret = sd_event_default(&event);
    if (ret < 0) {
        fprintf(stderr, "Failed to create default event loop: %s\n", strerror(-ret));
        goto finish;
    }
    sigset_t ss;
    if (sigemptyset(&ss) < 0 || sigaddset(&ss, SIGTERM) < 0 || sigaddset(&ss, SIGINT) < 0) {
        fprintf(stderr, "Failed to set the signal set: %s\n", strerror(-ret));
        goto finish;
    }
    /* Block SIGTERM first, so that the event loop can handle it */
    if (sigprocmask(SIG_BLOCK, &ss, NULL) < 0) {
        fprintf(stderr, "Failed to block the signals: %s\n", strerror(-ret));
        goto finish;
    }
    /* Let's make use of the default handler and "floating" reference features of sd_event_add_signal() */
    ret = sd_event_add_signal(event, NULL, SIGTERM, event_handler, activeTransactions);
    if (ret < 0) {
        fprintf(stderr, "Could not add signal handler for SIGTERM to event loop: %s\n", strerror(-ret));
        goto finish;
    }
    ret = sd_event_add_signal(event, NULL, SIGINT, event_handler, activeTransactions);
    if (ret < 0) {
        fprintf(stderr, "Could not add signal handler for SIGINT to event loop: %s\n", strerror(-ret));
        goto finish;
    }
    ret = sd_bus_attach_event(bus, event, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not add sd-bus handling to event bus: %s\n", strerror(-ret));
        goto finish;
    }

    ret = sd_event_loop(event);
    if (ret < 0) {
        fprintf(stderr, "Error while running event loop: %s\n", strerror(-ret));
        goto finish;
    }

finish:
    while (activeTransactions && activeTransactions->id != NULL) {
        TransactionEntry* nextTransaction = activeTransactions->next;
        free(activeTransactions->id);
        free(activeTransactions);
        activeTransactions = nextTransaction;
    }
    free(activeTransactions);
    sd_event_unref(event);
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);

    return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
