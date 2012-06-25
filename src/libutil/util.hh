#ifndef __UTIL_H
#define __UTIL_H

#include "types.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>

#include <cstdio>


namespace nix {


#define foreach(it_type, it, collection)                                \
    for (it_type it = (collection).begin(); it != (collection).end(); ++it)


/* Return an environment variable. */
string getEnv(const string & key, const string & def = "");

/* Return an absolutized path, resolving paths relative to the
   specified directory, or the current directory otherwise.  The path
   is also canonicalised. */
Path absPath(Path path, Path dir = "");

/* Canonicalise a path by removing all `.' or `..' components and
   double or trailing slashes.  Optionally resolves all symlink
   components such that each component of the resulting path is *not*
   a symbolic link. */
Path canonPath(const Path & path, bool resolveSymlinks = false);

/* Return the directory part of the given canonical path, i.e.,
   everything before the final `/'.  If the path is the root or an
   immediate child thereof (e.g., `/foo'), this means an empty string
   is returned. */
Path dirOf(const Path & path);

/* Return the base name of the given canonical path, i.e., everything
   following the final `/'. */
string baseNameOf(const Path & path);

/* Get status of `path'. */
struct stat lstat(const Path & path);

/* Return true iff the given path exists. */
bool pathExists(const Path & path);

/* Read the contents (target) of a symbolic link.  The result is not
   in any way canonicalised. */
Path readLink(const Path & path);

bool isLink(const Path & path);

/* Read the contents of a directory.  The entries `.' and `..' are
   removed. */
Strings readDirectory(const Path & path);

/* Read the contents of a file into a string. */
string readFile(int fd);
string readFile(const Path & path);

/* Write a string to a file. */
void writeFile(const Path & path, const string & s);

/* Read a line from a file descriptor. */
string readLine(int fd);

/* Write a line to a file descriptor. */
void writeLine(int fd, string s);

/* Compute the sum of the sizes of all files in `path'. */
void computePathSize(const Path & path,
    unsigned long long & bytes, unsigned long long & blocks);

/* Delete a path; i.e., in the case of a directory, it is deleted
   recursively.  Don't use this at home, kids.  The second variant
   returns the number of bytes and blocks freed. */
void deletePath(const Path & path);

void deletePath(const Path & path, unsigned long long & bytesFreed,
    unsigned long long & blocksFreed);

/* Make a path read-only recursively. */
void makePathReadOnly(const Path & path);

/* Create a temporary directory. */
Path createTempDir(const Path & tmpRoot = "", const Path & prefix = "nix",
    bool includePid = true, bool useGlobalCounter = true);

/* Create a directory and all its parents, if necessary.  Returns the
   list of created directories, in order of creation. */
Paths createDirs(const Path & path);


template<class T, class A>
T singleton(const A & a)
{
    T t;
    t.insert(a);
    return t;
}


/* Messages. */


typedef enum {
    ltPretty,   /* nice, nested output */
    ltEscapes,  /* nesting indicated using escape codes (for log2xml) */
    ltFlat      /* no nesting */
} LogType;

extern LogType logType;
extern Verbosity verbosity; /* suppress msgs > this */

class Nest
{
private:
    bool nest;
public:
    Nest();
    ~Nest();
    void open(Verbosity level, const format & f);
    void close();
};

void printMsg_(Verbosity level, const format & f);

#define startNest(varName, level, f) \
    Nest varName; \
    if (level <= verbosity) { \
      varName.open(level, (f)); \
    }

#define printMsg(level, f) \
    do { \
        if (level <= verbosity) { \
            printMsg_(level, (f)); \
        } \
    } while (0)

#define debug(f) printMsg(lvlDebug, f)

void warnOnce(bool & haveWarned, const format & f);

extern void (*writeToStderr) (const unsigned char * buf, size_t count);


/* Wrappers arount read()/write() that read/write exactly the
   requested number of bytes. */
void readFull(int fd, unsigned char * buf, size_t count);
void writeFull(int fd, const unsigned char * buf, size_t count);

MakeError(EndOfFile, Error)


/* Read a file descriptor until EOF occurs. */
string drainFD(int fd);



/* Automatic cleanup of resources. */


template <class T>
struct AutoDeleteArray
{
    T * p;
    AutoDeleteArray(T * p) : p(p) { }
    ~AutoDeleteArray() 
    {
        delete [] p;
    }
};


class AutoDelete
{
    Path path;
    bool del;
    bool recursive;    
public:
    AutoDelete(const Path & p, bool recursive = true);
    ~AutoDelete();
    void cancel();
};


class AutoCloseFD
{
    int fd;
public:
    AutoCloseFD();
    AutoCloseFD(int fd);
    AutoCloseFD(const AutoCloseFD & fd);
    ~AutoCloseFD();
    void operator =(int fd);
    operator int() const;
    void close();
    bool isOpen();
    int borrow();
};


class Pipe
{
public:
    AutoCloseFD readSide, writeSide;
    void create();
};


class AutoCloseDir
{
    DIR * dir;
public:
    AutoCloseDir();
    AutoCloseDir(DIR * dir);
    ~AutoCloseDir();
    void operator =(DIR * dir);
    operator DIR *();
    void close();
};


class Pid
{
    pid_t pid;
    bool separatePG;
    int killSignal;
public:
    Pid();
    ~Pid();
    void operator =(pid_t pid);
    operator pid_t();
    void kill();
    int wait(bool block);
    void setSeparatePG(bool separatePG);
    void setKillSignal(int signal);
};


/* Kill all processes running under the specified uid by sending them
   a SIGKILL. */
void killUser(uid_t uid);


/* Run a program and return its stdout in a string (i.e., like the
   shell backtick operator). */
string runProgram(Path program, bool searchPath = false,
    const Strings & args = Strings());

/* Close all file descriptors except stdin, stdout, stderr, and those
   listed in the given set.  Good practice in child processes. */
void closeMostFDs(const set<int> & exceptions);

/* Set the close-on-exec flag for the given file descriptor. */
void closeOnExec(int fd);

/* Wrapper around _exit() on Unix and ExitProcess() on Windows.  (On
   Cygwin, _exit() doesn't seem to do the right thing.) */
void quickExit(int status);

/* Common initialisation for setuid programs: clear the environment,
   sanitize file handles 0, 1 and 2. */
void setuidCleanup();


/* User interruption. */

extern volatile sig_atomic_t _isInterrupted;

void _interrupted();

void inline checkInterrupt()
{
    if (_isInterrupted) _interrupted();
}

MakeError(Interrupted, BaseError)


/* String tokenizer. */
Strings tokenizeString(const string & s, const string & separators = " \t\n\r");


/* Concatenate the given strings with a separator between the
   elements. */
string concatStringsSep(const string & sep, const Strings & ss);


/* Convert the exit status of a child as returned by wait() into an
   error string. */
string statusToString(int status);

bool statusOk(int status);


/* Parse a string into an integer. */
template<class N> bool string2Int(const string & s, N & n)
{
    std::istringstream str(s);
    str >> n;
    return str && str.get() == EOF;
}

string int2String(int n);


/* Return true iff `s' ends in `suffix'. */
bool hasSuffix(const string & s, const string & suffix);


/* Read string `s' from stream `str'. */
void expect(std::istream & str, const string & s);


/* Read a C-style string from stream `str'. */
string parseString(std::istream & str);


/* Utility function used to parse legacy ATerms. */
bool endOfList(std::istream & str);


/* Exception handling in destructors: print an error message, then
   ignore the exception. */
void ignoreException();


}


#endif /* !__UTIL_H */