#ifndef JNI2HOOK_VISITORS_INSTRUCTION_H
#define JNI2HOOK_VISITORS_INSTRUCTION_H

#include "visitor.h"

/* A decoded bytecode instruction, JVMS chapter 6.
 *
 * Branch targets are kept as absolute offsets into the method rather than as
 * the relative deltas the format stores. Nothing is lost, the delta is just
 * target minus the offset of the instruction, and it means that inserting or
 * removing an instruction later only requires recomputing offsets: every jump
 * keeps pointing at the same instruction without any of them being rewritten by
 * hand.
 *
 * Three encodings do not follow the opcode length table:
 *   wide          a prefix that widens the operand of the opcode after it
 *   tableswitch   padded to a four byte boundary, then a run of targets
 *   lookupswitch  padded the same way, then a run of match/target pairs
 * The padding counts from the start of the code array, not from the switch, so
 * an instruction's encoded length depends on where it sits. */

typedef enum
{
    OPERAND_NONE,
    OPERAND_IMMEDIATE,
    OPERAND_LOCAL,
    OPERAND_IINC,
    OPERAND_CONSTANT,
    OPERAND_CP_INDEX,
    OPERAND_INVOKE_INTERFACE,
    OPERAND_INVOKE_DYNAMIC,
    OPERAND_MULTIANEWARRAY,
    OPERAND_BRANCH,
    OPERAND_BRANCH_WIDE,
    OPERAND_TABLE_SWITCH,
    OPERAND_LOOKUP_SWITCH
} operand_kind;

typedef struct
{
    i4 match;
    i4 target;
} switch_pair;

typedef struct
{
    u4 offset;
    u1 opcode;
    bool wide;
    operand_kind kind;
    union
    {
        struct
        {
            i4 value;
        } immediate;
        struct
        {
            u2 index;
        } local;
        struct
        {
            u2 index;
            i4 constant;
        } iinc;
        struct
        {
            u2 index;
        } constant;
        struct
        {
            u2 index;
        } cp;
        struct
        {
            u2 index;
            u1 count;
            u1 zero;
        } invoke_interface;
        struct
        {
            u2 index;
            u2 zero;
        } invoke_dynamic;
        struct
        {
            u2 index;
            u1 dimensions;
        } multianewarray;
        struct
        {
            i4 target;
        } branch;
        struct
        {
            i4 default_target;
            i4 low;
            i4 high;
            i4 *targets;
        } table_switch;
        struct
        {
            i4 default_target;
            i4 count;
            switch_pair *pairs;
        } lookup_switch;
    } u;
} instruction;

operand_kind instruction_kind_of(u1 opcode);
void instruction_free(instruction *node);

/* Encoded length at a given offset. The offset matters only for the two
   switches, whose padding depends on where they land. */
u4 instruction_length_at(const instruction *node, u4 offset);

typedef struct
{
    instruction *items;
    u4 count;
    u4 capacity;
    u4 code_length;
} instruction_list;

void instruction_list_init(instruction_list *list);
void instruction_list_free(instruction_list *list);
classfile_status instruction_list_reserve(instruction_list *list, u4 capacity);
classfile_status instruction_list_append(instruction_list *list, instruction **out);

classfile_status instruction_list_parse(const u1 *code, u4 code_length, instruction_list *out);
classfile_status instruction_list_encode(const instruction_list *list, u1 **out, u4 *out_length);

/* Recomputes every offset from the current instruction order. Needed after an
   insertion, and it may have to run more than once because a switch can change
   length when its padding shifts, which moves everything behind it again. */
void instruction_list_recompute_offsets(instruction_list *list);

const instruction *instruction_list_at_offset(const instruction_list *list, u4 offset);

#endif
