#include "exportd_handler.hpp"
#include <core123/throwutils.hpp>
#include <core123/syslog_number.hpp>
#include <core123/diag.hpp>
#include <core123/sew.hpp>
#include <core123/log_channel.hpp>

using namespace core123;

namespace{
char PROGNAME[] = "exportd_handler";
}

int app_exportd(int argc, char *argv[]) try
{
    the_diag().opt_tid = true;
    // There is one option_parser.
    core123::option_parser op;
    // Associate the option_parser with instances of the generic
    // server_options and the specific exportd_options.
    fs123p7::server_options server_opts(op);
    exportd_options exportd_opts(op, PROGNAME);
    // Parse all options together, populating server_opts
    // and exportd_opts
    auto more_args = op.setopts_from_argv(argc, argv);
    // Help only?
    if(exportd_opts.help){
        std::cerr << op.helptext() << "\n";
        return 0;
    }
    if(!more_args.empty())
        throw std::runtime_error("unrecognized arguments:" + strbe(more_args));
    exportd_global_setup(exportd_opts);
    exportd_handler h(exportd_opts);
    std::unique_ptr<fs123p7::server> s;
    std::unique_ptr<fs123p7::tp_handler<exportd_handler>> tph;
    if(exportd_opts.threadpool_max){
        tph = std::make_unique<fs123p7::tp_handler<exportd_handler>>(exportd_opts.threadpool_max,
                                                            exportd_opts.threadpool_idle, h);
        s = std::make_unique<fs123p7::server>(server_opts, *tph);
    }else{
        s = std::make_unique<fs123p7::server>(server_opts, h);
    }
    s->set_signal_handlers(); // stop on TERM, INT, HUP and QUIT
    s->add_sig_handler(SIGUSR1,
                      [&](int, void*){
                          complain(LOG_NOTICE, "caught SIGUSR1.  Re-opening accesslog and complaint log");
                          h.accesslog_channel.reopen();
                          reopen_complaint_destination();
                      },
                      nullptr);
    if(exportd_opts.argcheck)
        return 0;
    if(!exportd_opts.portfile.empty()){
        std::ofstream ofs(exportd_opts.portfile.c_str());
        sockaddr_in sain = s->get_sockaddr_in();
        ofs << ntohs(sain.sin_port) << "\n";
        ofs.close();
        if(!ofs)
            throw se("Could not write to portfile");
    }
    s->run(); // normally runs forever.
    return 0;
 }catch(std::exception& e){
    core123::complain(e, "Shutting down because of exception caught in main");
    return 1;
 }
