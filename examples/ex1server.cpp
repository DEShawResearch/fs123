#include "fs123/fs123server.hpp"
#include <core123/svto.hpp>
#include <core123/opt.hpp>
#include <core123/strutils.hpp>

using namespace core123;

// Nothing ever changes, so we can use the same validator,
// estale_cookie and etag everywhere.  There's no reason
// for them to be different, except to make it a little
// more obvious for debugging.
static constexpr uint64_t validator = 12345;
static constexpr uint64_t estale_cookie = 54321;
static constexpr uint64_t etag = 31415;

struct bench_handler: public fs123p7::handler_base{
    // N.B.  The cc could be a constructor-argument...
    std::string cc = "max-age:10,stale-while-revalidate=20,stale-if-error=60";
    bool strictly_synchronous() override { return true; }
    void a(fs123p7::req::up reqp) override {
        struct stat sb = {}; // all zeros!
        if(reqp->path_info.empty()){
            // asking about the root.  It's executable but niether
            // readable nor writable.
            sb.st_mode = S_IFDIR | 0111;
        }else{
            // Other than the root, the only entries are regular files
            // whose names can be parsed as numbers.
            sb.st_mode = S_IFREG | 0444;
            try{
                sb.st_size = svto<size_t>(reqp->path_info, 1);
            }catch(std::exception&){
                return errno_reply(std::move(reqp), ENOENT, cc);
            }
        }
        a_reply(std::move(reqp), sb, validator, estale_cookie, cc);
    }
    void d(fs123p7::req::up reqp, uint64_t /*inm64*/, bool /*begin*/, int64_t /*offset*/) override{
        // There are files here, but you can't list them.
        d_reply(std::move(reqp), true, validator, estale_cookie, cc);
    }
    void f(fs123p7::req::up reqp, uint64_t inm64, size_t len, uint64_t offset, void* buf) override{
        if(inm64 == etag)
            return not_modified_reply(std::move(reqp), cc);
        size_t sz;
        // The only files that exist have names that can be parsed as
        // numbers.  Their contents is the letter 'x' repeated
        // <filename> times.
        try{
            sz = svto<size_t>(reqp->path_info, 1);
        }catch(std::exception&){
            return errno_reply(std::move(reqp), ENOENT, cc);
        }
        size_t n;
        if(offset > sz)
            n = 0;
        else if(len + offset > sz)
            n = sz - offset;
        else
            n = len;
        ::memset(buf, 'x', n);
        f_reply(std::move(reqp), n, validator, etag, estale_cookie, cc);
    }
    void l(fs123p7::req::up reqp) override{
        errno_reply(std::move(reqp), ENOENT, cc);
    }
    void s(fs123p7::req::up reqp) override{
        errno_reply(std::move(reqp), ENOTSUP, cc);
    }

    bench_handler(){}
    ~bench_handler(){}
};

int main(int argc, char *argv[]) try
{
    // Associate an option_parser with instances of the generic
    // server_options.
    core123::option_parser opt_parser;
    fs123p7::server_options server_opts(opt_parser);
    // Add a few more options of our own:
    bool help=false;
    opt_parser.add_option("help", "produce this message", opt_true_setter(help));
    int threadpool_max;
    opt_parser.add_option("threadpool_max", "0", "maximum number of handler threads", opt_setter(threadpool_max));
    int threadpool_idle;
    opt_parser.add_option("threadpool_idle", "1", "number of handler threads at zero load", opt_setter(threadpool_idle));

    // Parse all options together, populating both server_opts and
    // our own options.
    auto more_args = opt_parser.setopts_from_argv(argc, argv);
    // Help only?
    if(help){
        std::cerr << opt_parser.helptext() << "\n";
        return 0;
    }
    if(!more_args.empty())
        throw std::runtime_error("unrecognized arguments:" + strbe(more_args));

    // Create a bench_handler, optionally wrap it with a threadpool wrapper,
    // and then create a server with the handler.
    bench_handler h;
    std::unique_ptr<fs123p7::tp_handler<bench_handler>> tph;
    std::unique_ptr<fs123p7::server> s;
    if(threadpool_max){
        tph = std::make_unique<fs123p7::tp_handler<bench_handler>>(threadpool_max,
                                                                   threadpool_idle, h);
        s = std::make_unique<fs123p7::server>(server_opts, *tph);
    }else{
        s = std::make_unique<fs123p7::server>(server_opts, h);
    }
    // run the server (forever - until we get kill()-ed)
    s->run();
    return 0;
 }catch(std::exception& e){
    core123::complain(e, "Shutting down because of exception caught in main");
    return 1;
 }
