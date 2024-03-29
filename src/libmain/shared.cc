#include "config.h"

#include "shared.hh"
#include "globals.hh"
#include "store-api.hh"
#include "util.hh"
#include "misc.hh"

#include <iostream>
#include <cctype>
#include <exception>

#include <sys/stat.h>
#include <unistd.h>

#if HAVE_BOEHMGC
#include <gc/gc.h>
#endif


namespace nix {


volatile sig_atomic_t blockInt = 0;


static void sigintHandler(int signo)
{
    if (!blockInt) {
        _isInterrupted = 1;
        blockInt = 1;
    }
}


void printGCWarning()
{
    static bool haveWarned = false;
    warnOnce(haveWarned,
        "you did not specify `--add-root'; "
        "the result might be removed by the garbage collector");
}


void printMissing(StoreAPI & store, const PathSet & paths)
{
    unsigned long long downloadSize, narSize;
    PathSet willBuild, willSubstitute, unknown;
    queryMissing(store, paths, willBuild, willSubstitute, unknown, downloadSize, narSize);
    printMissing(willBuild, willSubstitute, unknown, downloadSize, narSize);
}


void printMissing(const PathSet & willBuild,
    const PathSet & willSubstitute, const PathSet & unknown,
    unsigned long long downloadSize, unsigned long long narSize)
{
    if (!willBuild.empty()) {
        printMsg(lvlInfo, format("these derivations will be built:"));
        foreach (PathSet::iterator, i, willBuild)
            printMsg(lvlInfo, format("  %1%") % *i);
    }

    if (!willSubstitute.empty()) {
        printMsg(lvlInfo, format("these paths will be fetched (%.2f MiB download, %.2f MiB unpacked):")
            % (downloadSize / (1024.0 * 1024.0))
            % (narSize / (1024.0 * 1024.0)));
        foreach (PathSet::iterator, i, willSubstitute)
            printMsg(lvlInfo, format("  %1%") % *i);
    }

    if (!unknown.empty()) {
        printMsg(lvlInfo, format("don't know how to build these paths%1%:")
            % (settings.readOnlyMode ? " (may be caused by read-only store access)" : ""));
        foreach (PathSet::iterator, i, unknown)
            printMsg(lvlInfo, format("  %1%") % *i);
    }
}


static void setLogType(string lt)
{
    if (lt == "pretty") logType = ltPretty;
    else if (lt == "escapes") logType = ltEscapes;
    else if (lt == "flat") logType = ltFlat;
    else throw UsageError("unknown log type");
}


static bool showTrace = false;


string getArg(const string & opt,
    Strings::iterator & i, const Strings::iterator & end)
{
    ++i;
    if (i == end) throw UsageError(format("`%1%' requires an argument") % opt);
    return *i;
}


/* Initialize and reorder arguments, then call the actual argument
   processor. */
static void initAndRun(int argc, char * * argv)
{
    settings.processEnvironment();
    settings.loadConfFile();

    /* Catch SIGINT. */
    struct sigaction act;
    act.sa_handler = sigintHandler;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGINT, &act, 0))
        throw SysError("installing handler for SIGINT");
    if (sigaction(SIGTERM, &act, 0))
        throw SysError("installing handler for SIGTERM");
    if (sigaction(SIGHUP, &act, 0))
        throw SysError("installing handler for SIGHUP");

    /* Ignore SIGPIPE. */
    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    if (sigaction(SIGPIPE, &act, 0))
        throw SysError("ignoring SIGPIPE");

    /* Reset SIGCHLD to its default. */
    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    if (sigaction(SIGCHLD, &act, 0))
        throw SysError("resetting SIGCHLD");

    /* There is no privacy in the Nix system ;-)  At least not for
       now.  In particular, store objects should be readable by
       everybody. */
    umask(0022);

    /* Process the NIX_LOG_TYPE environment variable. */
    string lt = getEnv("NIX_LOG_TYPE");
    if (lt != "") setLogType(lt);

    /* Put the arguments in a vector. */
    Strings args, remaining;
    while (argc--) args.push_back(*argv++);
    args.erase(args.begin());

    /* Expand compound dash options (i.e., `-qlf' -> `-q -l -f'), and
       ignore options for the ATerm library. */
    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;
        if (arg.length() > 2 && arg[0] == '-' && arg[1] != '-' && !isdigit(arg[1])) {
            for (unsigned int j = 1; j < arg.length(); j++)
                if (isalpha(arg[j]))
                    remaining.push_back((string) "-" + arg[j]);
                else     {
                    remaining.push_back(string(arg, j));
                    break;
                }
        } else remaining.push_back(arg);
    }
    args = remaining;
    remaining.clear();

    /* Process default options. */
    int verbosityDelta = lvlInfo;
    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;
        if (arg == "--verbose" || arg == "-v") verbosityDelta++;
        else if (arg == "--quiet") verbosityDelta--;
        else if (arg == "--log-type") {
            string s = getArg(arg, i, args.end());
            setLogType(s);
        }
        else if (arg == "--no-build-output" || arg == "-Q")
            settings.buildVerbosity = lvlVomit;
        else if (arg == "--print-build-trace")
            settings.printBuildTrace = true;
        else if (arg == "--help") {
            printHelp();
            return;
        }
        else if (arg == "--version") {
            std::cout << format("%1% (Nix) %2%") % programId % nixVersion << std::endl;
            return;
        }
        else if (arg == "--keep-failed" || arg == "-K")
            settings.keepFailed = true;
        else if (arg == "--keep-going" || arg == "-k")
            settings.keepGoing = true;
        else if (arg == "--fallback")
            settings.set("build-fallback", "true");
        else if (arg == "--max-jobs" || arg == "-j")
            settings.set("build-max-jobs", getArg(arg, i, args.end()));
        else if (arg == "--cores")
            settings.set("build-cores", getArg(arg, i, args.end()));
        else if (arg == "--readonly-mode")
            settings.readOnlyMode = true;
        else if (arg == "--max-silent-time")
            settings.set("build-max-silent-time", getArg(arg, i, args.end()));
        else if (arg == "--timeout")
            settings.set("build-timeout", getArg(arg, i, args.end()));
        else if (arg == "--no-build-hook")
            settings.useBuildHook = false;
        else if (arg == "--show-trace")
            showTrace = true;
        else if (arg == "--option") {
            ++i; if (i == args.end()) throw UsageError("`--option' requires two arguments");
            string name = *i;
            ++i; if (i == args.end()) throw UsageError("`--option' requires two arguments");
            string value = *i;
            settings.set(name, value);
        }
        else remaining.push_back(arg);
    }

    verbosity = (Verbosity) (verbosityDelta < 0 ? 0 : verbosityDelta);

    settings.update();

    run(remaining);

    /* Close the Nix database. */
    store.reset((StoreAPI *) 0);
}


/* Called when the Boehm GC runs out of memory. */
static void * oomHandler(size_t requested)
{
    /* Convert this to a proper C++ exception. */
    throw std::bad_alloc();
}


void showManPage(const string & name)
{
    string cmd = "man " + name;
    if (system(cmd.c_str()) != 0)
        throw Error(format("command `%1%' failed") % cmd);
}


int exitCode = 0;
char * * argvSaved = 0;

}


static char buf[1024];

int main(int argc, char * * argv)
{
    using namespace nix;

    argvSaved = argv;

    /* Turn on buffering for cerr. */
#if HAVE_PUBSETBUF
    std::cerr.rdbuf()->pubsetbuf(buf, sizeof(buf));
#endif

    std::ios::sync_with_stdio(false);

#if HAVE_BOEHMGC
    /* Initialise the Boehm garbage collector.  This isn't necessary
       on most platforms, but for portability we do it anyway. */
    GC_INIT();

    GC_oom_fn = oomHandler;
#endif

    try {
        try {
            initAndRun(argc, argv);
        } catch (...) {
            /* Subtle: we have to make sure that any `interrupted'
               condition is discharged before we reach printMsg()
               below, since otherwise it will throw an (uncaught)
               exception. */
            blockInt = 1; /* ignore further SIGINTs */
            _isInterrupted = 0;
            throw;
        }
    } catch (UsageError & e) {
        printMsg(lvlError,
            format(
                "error: %1%\n"
                "Try `%2% --help' for more information.")
            % e.what() % programId);
        return 1;
    } catch (BaseError & e) {
        printMsg(lvlError, format("error: %1%%2%") % (showTrace ? e.prefix() : "") % e.msg());
        if (e.prefix() != "" && !showTrace)
            printMsg(lvlError, "(use `--show-trace' to show detailed location information)");
        return e.status;
    } catch (std::exception & e) {
        printMsg(lvlError, format("error: %1%") % e.what());
        return 1;
    }

    return exitCode;
}
