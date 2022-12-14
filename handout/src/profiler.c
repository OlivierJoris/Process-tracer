/*
 * Module implementing the profiler interface.
 * 
 * @author Maxime Goffart (180521) & Olivier Joris (182113)
 */

#include "profiler.h"
#include "functions_addresses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>

/*
 * Number of spaces representing a level of depth
 * inside the call tree.
 */
#define NB_BLANKS 4

/*
 * Represents a call to a function.
 */
typedef struct Func_call_t Func_call;
struct Func_call_t{
    Func_call* prev;                // Previous called function (parent)
    unsigned long eipBeforeCall;    // EIP before calling this function
    char* name;                     // Name of the function
    unsigned int nbInstr;           // Number of instructions (including from children)
    unsigned int nbRecCalls;        // Number of rec calls (0 -> no recursivity)
    Func_call* child;               // Child function
    Func_call* next;                // Next function
    unsigned int depth;             // Depth inside the call tree
};

struct Profiler_t{
    char* tracee;               // Tracee's name
    pid_t childPID;             // PID of tracee
    Func_call* entryPoint;      // Entry point of the call tree
};

/*
 * Initializes the profiler.
 * 
 * @param tracee Path to the executable of the tracee.
 * 
 * @return Empty profiling data.
 */
static Profiler* init_profiler(char* tracee);

/*
 * Traces the function calls for the given profiler.
 * 
 * @param profiler Profiler for which we want to
 * trace the function calls.
 */
static void trace_function_calls(Profiler* profiler);

/*
 * Creates a node representing a function call.
 * 
 * @return New node.
 */
static Func_call* func_call_create_node(void);

/*
 * Sets the fields of the new node.
 * 
 * @param new Node to be set.
 * @param prev Link to parent of new node.
 * @param newDepth Depth of the new node.
 * @param symbol Symbol of function represented by the new node.
 * @param prevEIP EIP before calling the function.
 */
static void func_call_set(
    Func_call* new,
    Func_call* prev,
    unsigned int newDepth,
    char* symbol,
    unsigned long prevEIP
);

/*
 * Increases the number of instructions starting from fc
 * up to the root of the tree.
 *
 * @param fc Node at which to start the increase.
 */
static void func_call_increase_nb_instr(Func_call* fc);

/*
 * Frees the memory associated to the function calls.
 * 
 * @param fc Beginning of the call tree.
 */
static void func_call_free(Func_call* fc);

/*
 * Prints the function calls as in the brief.
 * 
 * @param fc Beginning of the call tree.
 */
static void func_call_print(Func_call* fc);

/*
 * Prints an unique function as in the brief.
 * 
 * @param fc Function to print.
 */
static void func_call_print_unique(Func_call* fc);

/*
 * Rebuilds depths inside the call tree after a recursive function.
 * 
 * @param fc Function in the call tree after the recursive function
 * that we are considering.
 * @param minDepth Depth of the recursive function in the call tree.
 * @param currentDepth Current depth inside the tree.
 */
static void rebuild_depth(
    Func_call* fc,
    unsigned int minDepth,
    unsigned int currentDepth
);

Profiler* run_profiler(char* tracee){
    Profiler* profiler = init_profiler(tracee);
    if(!profiler){
        fprintf(stderr, "Unable to initialize profiler!\n");
        return NULL;
    }

    pid_t childPID = fork();
    if(childPID < 0){
        fprintf(stderr, "Failed forking process!\n");
        profiler_clean(profiler);
        return NULL;
    }else if(childPID == 0){
        // Allows tracer to trace the child process.
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        char* argv[] = {NULL};
        char* env[] = {NULL};
        close(1); // Doesn't show tracee output
        execve(profiler->tracee, argv, env);
    }else{
        profiler->childPID = childPID;
        trace_function_calls(profiler);
    }

    return profiler;
}

static void trace_function_calls(Profiler* profiler){
    if(!profiler){
        fprintf(stderr, "Profiler issue for tracing the function calls!\n");
        return;
    }

    // Loads mapping between functions names' and addresses
    FunctionsAddresses* fa = functions_addresses_load(profiler->tracee);
    if(!fa){
        fprintf(stderr, "Unable to load the mapping between functions names' "
                        "and addresses!\n");
        return;
    }

    int status;
    struct user_regs_struct userRegs;
    bool nextIsCallee = false, nextIsRet = false, reachedEntryPoint = false;
    unsigned long depth = 0, prevDepth = 0, prevEIP;
    // Used to get opcode on 1 byte
    const unsigned long PREFIX = 255;
    // Used to get opcode on 2 bytes
    const unsigned long PREFIX2 = 65535;

    // Initializing the call tree
    profiler->entryPoint = func_call_create_node();
    if(!profiler->entryPoint){
        fprintf(stderr, "Unable to initialize the call tree for the "
                        "profiler!\n");
        functions_addresses_clean(fa);
        return;
    }
    profiler->entryPoint->eipBeforeCall = 0;
    
    Func_call* currNode = profiler->entryPoint;
    Func_call* toBeUpdated = currNode;

    char* prevFuncName = calloc(256, sizeof(char));
    if(!prevFuncName){
        fprintf(stderr, "Unable to allocate memory for tracing function "
                        "calls!\n");
        functions_addresses_clean(fa);
        return;
    }

    while(true){
        // Checks if tracee process ended or not
        wait(&status);
        if(WIFEXITED(status))
            break;

        // Gets registers
        ptrace(PTRACE_GETREGS, profiler->childPID, NULL, &userRegs);

        // Detects entry point
        if(!reachedEntryPoint){
            char* symbol = functions_addresses_get_symbol(fa, userRegs.eip);
            if(symbol && (!strcmp(symbol, "main\0") || !strcmp(symbol, "_start\0"))){
                currNode->next = func_call_create_node();
                if(!currNode->next){
                    fprintf(stderr, "Unable to allocate memory to trace "
                                                "function calls!\n");
                    functions_addresses_clean(fa);
                    return;
                }
                func_call_set(currNode->next, currNode, depth, symbol, userRegs.eip);
                currNode = currNode->next;
                toBeUpdated = currNode;
                prevDepth = depth;
                depth+=1;
                reachedEntryPoint = true;
            }
        }

        // If previous instruction was RET
        if(nextIsRet){
            Func_call* tmp = currNode;
            // Needs to find new depth
            while(tmp){
                if((unsigned long)userRegs.eip >= tmp->eipBeforeCall+1 && 
                    (unsigned long)userRegs.eip <= tmp->eipBeforeCall+8){
                        depth = tmp->depth;
                        toBeUpdated = tmp->prev;
                        break;
                }else
                    toBeUpdated = currNode;
                tmp = tmp->prev;
            }
            if(toBeUpdated && toBeUpdated->name && currNode && currNode->name &&
                !strcmp(toBeUpdated->name, currNode->name)){
                while(toBeUpdated && toBeUpdated->name &&
                    !strcmp(toBeUpdated->name, currNode->name))
                    toBeUpdated = toBeUpdated->prev;
            }

            if(currNode && currNode->prev &&
                strcmp(currNode->name, currNode->prev->name) &&
                (unsigned long)userRegs.eip >= currNode->prev->eipBeforeCall+1 &&
                (unsigned long)userRegs.eip <= currNode->prev->eipBeforeCall+8)
                    toBeUpdated = currNode->prev;

            nextIsRet = false;
        }

        // If previous instruction was CALL
        if(nextIsCallee){
            // Gets symbol of function
            char* symbol = functions_addresses_get_symbol(fa, userRegs.eip);
            if(!symbol){
                fprintf(stderr, "Error while fetching function's name!\n");
                functions_addresses_clean(fa);
                free(prevFuncName);
                return;
            }

            // Updates structure of the tree
            Func_call* tmp_next;
            // Recursive function
            if(prevFuncName && depth == prevDepth + 1 && symbol && 
               !strcmp(symbol, prevFuncName)){
                currNode->nbRecCalls+=1;
                tmp_next = currNode;
            }else{
                // Needs to update next field.
                if(depth == currNode->depth || depth == currNode->depth - 1){
                    if(depth == currNode->depth - 1){
                        // Go back until we reach same depth
                        while(currNode->depth > depth)
                            currNode = currNode->prev;
                    }

                    currNode->next = func_call_create_node();
                    if(!currNode->next){
                        fprintf(stderr, "Unable to allocate memory to trace "
                                        "function calls!\n");
                        functions_addresses_clean(fa);
                        free(prevFuncName);
                        return;
                    }
                    tmp_next = currNode->next;
                }else{
                    // Needs to update child field.
                    currNode->child = func_call_create_node();
                    if(!currNode->child){
                        fprintf(stderr, "Unable to allocate memory to trace "
                                        "function calls!\n");
                        functions_addresses_clean(fa);
                        free(prevFuncName);
                        return;
                    }
                    tmp_next = currNode->child;
                }

                // Sets fields of new node (Func_call)
                if(symbol){
                    func_call_set(tmp_next, currNode, depth, symbol, prevEIP);
                    strcpy(prevFuncName, symbol);
                }else{
                    func_call_set(tmp_next, currNode, depth, "not found\0", prevEIP);
                    strcpy(prevFuncName, "not found\0");
                }
            }

            prevDepth = depth;
            currNode = tmp_next;
            toBeUpdated = currNode;
            nextIsCallee = false;
            depth+=1;
        }

        // Gets instruction - EIP = 32 bits instruction register
        unsigned long instr = ptrace(PTRACE_PEEKTEXT, profiler->childPID, 
                                     userRegs.eip, NULL);

        // Opcode (on 1 byte) is the last 2 hex digits because ptrace uses big-endian
        unsigned long opcode = instr & PREFIX;
        // Opcode (on 2 bytes) is the last 4 hex digits because ptrace uses big-endian
        unsigned long opcode2 = instr & PREFIX2;

        // Opcodes for CALL
        if(opcode == 0xe8){
            nextIsCallee = true;
            prevEIP = userRegs.eip; // address of instruction before call
        }

        // Opcodes for RET
        if(opcode == 0xc2 || opcode == 0xc3 || opcode == 0xca || 
           opcode == 0xcb || opcode2 == 0xc3f3 || opcode2 == 0xc3f2)
               nextIsRet = true;

        // Updates number of instructions
        func_call_increase_nb_instr(toBeUpdated);

        // Next instruction
        ptrace(PTRACE_SINGLESTEP, profiler->childPID, 0, 0);
    }

    free(prevFuncName);

    functions_addresses_clean(fa);

    return;
}

void profiler_clean(Profiler* profiler){
    if(!profiler)
        return;
    if(profiler->tracee)
        free(profiler->tracee);
    if(profiler->entryPoint)
        func_call_free(profiler->entryPoint);
    free(profiler);
    return;
}

void profiler_display_data(Profiler* profiler){
    if(profiler->entryPoint && profiler->entryPoint->next)
        func_call_print(profiler->entryPoint->next);
    return;
}

static Profiler* init_profiler(char* tracee){
    if(!tracee){
        fprintf(stderr, "Error given tracee!\n");
        return NULL;
    }

    Profiler* profiler = malloc(sizeof(Profiler));
    if(!profiler){
        fprintf(stderr, "Unable to allocate memory for profiler!\n");
        return NULL;
    }

    size_t strLen = strlen(tracee) + 1; // + 1 for '\0'
    profiler->tracee = malloc(sizeof(char) * strLen);
    if(!profiler->tracee){
        fprintf(stderr, "Unable to allocate memory for profiler!\n");
        profiler_clean(profiler);
        return NULL;
    }
    strcpy(profiler->tracee, tracee);

    profiler->entryPoint = NULL;

    return profiler;
}

static Func_call* func_call_create_node(void){
    Func_call* node = malloc(sizeof(Func_call));
    if(!node)
        return NULL;

    node->prev = NULL;
    node->name = NULL;
    node->eipBeforeCall = 0;
    node->nbInstr = 0;
    node->nbRecCalls = 0;
    node->child = NULL;
    node->next = NULL;
    node->depth = 0;
    
    return node;
}

static void func_call_set(Func_call* new, Func_call* prev, unsigned int newDepth, 
                   char* symbol, unsigned long prevEIP){
    if(!new)
        return;

    new->prev = prev;
    new->depth = newDepth;
    new->eipBeforeCall = prevEIP;
    if(symbol){
        size_t length = strlen(symbol) + 1;
        new->name = malloc(sizeof(char) * length);
        if(!new->name){
            fprintf(stderr, "Unable to allocate memory for profiler!\n");
            return;
        }
        strcpy(new->name, symbol);
    }

    return;
}

static void func_call_increase_nb_instr(Func_call* fc){
    if(!fc)
        return;

    /*
     * Updates number of instructions from current node (fc)
     * up to root of the tree.
    */
    Func_call* tmp_prev = fc;
    unsigned int prevDepth = fc->depth;
    fc->nbInstr++; // Increases current node nb instr
    // Increases inside the rest of the tree
    while(tmp_prev && prevDepth != 0){
        while(tmp_prev && !(tmp_prev->depth <= prevDepth - 1))
            tmp_prev = tmp_prev->prev;

        if(tmp_prev){
            tmp_prev->nbInstr++;
            prevDepth = tmp_prev->depth;
        }
    }

    return;
}

static void rebuild_depth(Func_call* fc, unsigned int minDepth, 
                          unsigned int currentDepth){
    if(!fc)
        return;
    
    if(fc->depth == minDepth)
        return;
    else
        fc->depth = currentDepth;
    
    if(fc->child)
        rebuild_depth(fc->child, minDepth, currentDepth+1);

    if(fc->next)
        rebuild_depth(fc->next, minDepth, currentDepth);
}

static void func_call_print(Func_call* fc){
    if(!fc)
        return;
    
    Func_call* tmp = fc;
    while(tmp){
        func_call_print_unique(tmp);
        if(tmp->child)
            func_call_print(tmp->child);
        tmp = tmp->next;
    }
}

static void func_call_print_unique(Func_call* fc){
    if(!fc)
        return;
    for(unsigned int i = 0; i < NB_BLANKS * fc->depth; ++i)
        printf(" ");
    if(fc->name)
        printf("%s", fc->name);
    else
        printf("(null)\n");
    if(fc->nbRecCalls != 0){
        rebuild_depth(fc->child, fc->depth, fc->depth+1);
        printf(" [rec call: %u]", fc->nbRecCalls);
    }

    printf(": %u\n", fc->nbInstr);

    return;
}

static void func_call_free(Func_call* fc){
    if(!fc)
        return;

    Func_call* tmp = fc;
    while(tmp){
        if(tmp->name){
            free(tmp->name);
            tmp->name = NULL;
        }
        if(tmp->child){
            func_call_free(tmp->child);
            tmp->child = NULL;
        }
        Func_call* tmp2 = tmp->next;
        if(tmp){
            free(tmp);
            tmp = NULL;
        }
        tmp = tmp2;
    }
}
