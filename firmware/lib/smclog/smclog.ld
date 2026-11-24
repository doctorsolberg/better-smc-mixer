/*
 * smclog call sites (see smclog.h). INCLUDE this inside SECTIONS { } of an image's linker
 * script. INFO: the section stays in the ELF for the dictionary (tools/smclog) and takes no
 * memory on the device; IDs are offsets from __start_smclog.
 */
smclog 0 (INFO) :
{
    __start_smclog = .;
    KEEP(*(smclog))
    __stop_smclog = .;
}
