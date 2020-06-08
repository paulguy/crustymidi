#define CRUSTY_IO_READ_FUNC_DECL(X)   int (*X)(void *priv, \
                                               void *val, \
                                               unsigned int index)
#define CRUSTY_IO_WRITE_FUNC_DECL(X)  int (*X)(void *priv, \
                                               void *val, \
                                               unsigned int index)

#define CRUSTY_FLAG_DEFAULTS (0)
#ifdef CRUSTY_TEST
#define CRUSTY_FLAG_OUTPUT_PASSES (1<<0)
#endif
#define CRUSTY_FLAG_TRACE (1<<1)

typedef enum {
    CRUSTY_STATUS_READY = 0,
    CRUSTY_STATUS_ACTIVE = 1,
    CRUSTY_STATUS_INTERNAL_ERROR = 2,
    CRUSTY_STATUS_OUT_OF_RANGE = 3,
    CRUSTY_STATUS_INVALID_INSTRUCTION = 4,
    CRUSTY_STATUS_STACK_OVERFLOW = 5,
    CRUSTY_STATUS_CALLBACK_FAILED = 6,
    CRUSTY_STATUS_FLOAT_INDEX = 7,
    CRUSTY_STATUS_INVALID = 8
} CrustyStatus;

typedef enum {
    CRUSTY_TYPE_CHAR,
    CRUSTY_TYPE_INT,
    CRUSTY_TYPE_FLOAT
} CrustyType;

typedef struct {
    const char *name;
    unsigned int length;
    CrustyType type;

    CRUSTY_IO_READ_FUNC_DECL(read);
    void *readpriv;
    CRUSTY_IO_WRITE_FUNC_DECL(write);
    void *writepriv;
} CrustyCallback;

typedef struct CrustyVM_s CrustyVM;

/*
 * Load a program and prepare the VM to run.
 *
 * name             The name of the module.  Must be not NULL but only used for
 *                  messages passed to the user.
 * program          Buffer containing the program text.
 * len              Length of the program in bytes.
 * flags            Bitfield of flags which describe how certain things should
 *                  happen:
 *                  CRUSTY_FLAG_OUTPUT_PASSES - Output each pass to file to help
 *                                              in debugging and development.
 * callstacksize    Specify the callstack size.  This isn't the memory size but
 *                  the depth of procedures which could be called.
 * cb               Array of callbacks described by struct CrustyCallback.
                        name        Variable name which the callback will be
                                    called by.
                        size        The maximum value which should be passed in
                                    to the callback as an index.
                        read        The function to be called on reads:
                            priv    The private pointer provided as readpriv.
                            val     A pointer to an integer which the value
                                    should be set to.
                            index   The index requested by the program, 0 if no
                                    index is specified.
                            returns Negative to indicate a failure.
                        readpriv    A private pointer passed in to the read
                                    callback.
                        write       The function to be called on writes:
                            priv    The private pointer provided as writepriv.
                            val     The value passed in by the program.
                            index   The index requested by the program, 0 if no
                                    index is specified
                            returns Negative to indicate a failure.
 * cbcount          Number of callbacks in array.
 * log_cb           Callback for any information printed:
 *                      priv    Private pointer passed in by log_priv.
 *                      fmt     printf style format string.
 *                      ...     Arguments for format string.
 * log_priv         Private pointer passed in to log_cb.
 * returns          The newly loaded CrustyVM.
 */
CrustyVM *crustyvm_new(const char *name,
                       const char *program,
                       int len,
                       unsigned int flags,
                       unsigned int callstacksize,
                       const CrustyCallback *cb,
                       unsigned int cbcount,
                       void (*log_cb)(void *priv, const char *fmt, ...),
                       void *log_priv);

/*
 * Free memory allocated by cvm.
 *
 * cvm  CrustyVM to free memory from.
 */
void crustyvm_free(CrustyVM *cvm);

/*
 * Reset program state.  All memory is reinitialized.
 *
 * cvm      CrustyVM to reset.
 * returns  Negative on failure.
 */
int crustyvm_reset(CrustyVM *cvm);

/*
 * Prepare a program to run with an entry point.
 *
 * cvm      CrustyVM to prepare.
 * procname Name of procedure execution should begin at.
 * returns  Negative on failure.
 */
int crustyvm_begin(CrustyVM *cvm, const char *procname);

/*
 * Process the next instruction after a program has begun.
 *
 * cvm      CrustyVM to step.
 * returns  Negative on failure.
 */
CrustyStatus crustyvm_step(CrustyVM *cvm);

/*
 * Run a procedure until it is done or there is an error.
 *
 * cvm      CrustyVM to run.
 * procname Procedure to run.
 * returns  Negative on failure.
 */
int crustyvm_run(CrustyVM *cvm, const char *procname);

/*
 * Get status of CrustyVM.
 *
 * cvm      CrustyVM to get status from.
 * returns  Status code.
 */
CrustyStatus crustyvm_get_status(CrustyVM *cvm);

/*
 * Get string from status code.
 *
 * status   Status code.
 * return   String describing the status code.
 */
const char *crustyvm_statusstr(CrustyStatus status);

/*
 * Log a debug trace of the state of the VM.
 *
 * cvm      CrustyVM to log debug trace from.
 * long     Print full values of variables.
 */
void crustyvm_debugtrace(CrustyVM *cvm, int full);

/*
 * Check to see if a VM has a particular entry point.
 *
 * cvm      CrustyVm to check.
 * name     Name of entry point.
 */
int crustyvm_has_entrypoint(CrustyVM *cvm, const char *name);

