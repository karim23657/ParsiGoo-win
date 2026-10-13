// SAL-annotation shim for non-MSVC toolchains.
//
// onnxruntime_c_api.h is annotated with Microsoft's SAL macros (_In_, _Outptr_,
// _Frees_ptr_opt_, _Success_, ...). MSVC gets them from the compiler; MinGW's
// <sal.h> does not define all of them, so the header fails to compile. They are
// documentation-only annotations, so defining them to nothing is correct.
#pragma once

#if defined(_MSC_VER)
#include <windows.h>
#include <sal.h>
#else
#ifdef _WIN32
#include <windows.h>
#include <sal.h>

#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Frees_ptr_
#define _Frees_ptr_
#endif
#ifndef _Success_
#define _Success_(x)
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Ret_
#define _Ret_
#endif
#ifndef _Null_terminated_
#define _Null_terminated_
#endif
#ifndef _Field_size_
#define _Field_size_(x, y)
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(x)
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _Post_equal_to_
#define _Post_equal_to_(x)
#endif
#ifndef _Always_
#define _Always_
#endif
#ifndef _Notnull_
#define _Notnull_
#endif
#ifndef _Null_
#define _Null_
#endif
#ifndef _When_
#define _When_(x, y)
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif

#endif  // _WIN32
#endif