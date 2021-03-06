// -*- c++ -*-
// 
// Copyright (c) 2006      Los Alamos National Security, LLC.  All rights
//                         reserved. 
// Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
// Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
// $COPYRIGHT$
// 
// Additional copyrights may follow
// 
// $HEADER$
//

// do not include ompi_config.h because it kills the free/malloc defines
#include "mpi.h"
#include "ompi/mpi/cxx/mpicxx.h"
#include "opal/threads/mutex.h"

#include "ompi/communicator/communicator.h"
#include "ompi/attribute/attribute.h"
#include "ompi/errhandler/errhandler.h"

static void cxx_type_keyval_destructor(int keyval);

void
MPI::Datatype::Free()
{
    MPI_Datatype save = mpi_datatype;
    (void)MPI_Type_free(&mpi_datatype);

    OPAL_THREAD_LOCK(MPI::mpi_map_mutex);
    MPI::Datatype::mpi_type_map.erase(save);
    OPAL_THREAD_UNLOCK(MPI::mpi_map_mutex);
}


int
MPI::Datatype::do_create_keyval(MPI_Type_copy_attr_function* c_copy_fn,
                                MPI_Type_delete_attr_function* c_delete_fn,
                                Copy_attr_function* cxx_copy_fn,
                                Delete_attr_function* cxx_delete_fn,
                                void* extra_state)
{
    int keyval, ret, count = 0;
    ompi_attribute_fn_ptr_union_t copy_fn;
    ompi_attribute_fn_ptr_union_t delete_fn;
    Copy_attr_function *cxx_pair_copy = NULL;
    Delete_attr_function *cxx_pair_delete = NULL;

    // We do not call MPI_Type_create_keyval() here because we need to
    // pass in a special destructor to the backend keyval creation
    // that gets invoked when the keyval's reference count goes to 0
    // and is finally destroyed (i.e., clean up some caching/lookup
    // data here in the C++ bindings layer).  This destructor is
    // *only* used in the C++ bindings, so it's not set by the C
    // MPI_Type_create_keyval().  Hence, we do all the work here (and
    // ensure to set the destructor atomicly when the keyval is
    // created).

    // Error check.  Must have exactly 2 non-NULL function pointers.
    if (NULL != c_copy_fn) {
        copy_fn.attr_datatype_copy_fn = 
            (MPI_Type_internal_copy_attr_function*) c_copy_fn;
        ++count;
    }
    if (NULL != c_delete_fn) {
        delete_fn.attr_datatype_delete_fn = c_delete_fn;
        ++count;
    }
    if (NULL != cxx_copy_fn) {
        copy_fn.attr_datatype_copy_fn =
            (MPI_Type_internal_copy_attr_function*) ompi_mpi_cxx_type_copy_attr_intercept;
        cxx_pair_copy = cxx_copy_fn;
        ++count;
    }
    if (NULL != cxx_delete_fn) {
        delete_fn.attr_datatype_delete_fn =
            ompi_mpi_cxx_type_delete_attr_intercept;
        cxx_pair_delete = cxx_delete_fn;
        ++count;
    }
    if (2 != count) {
        return OMPI_ERRHANDLER_INVOKE(MPI_COMM_WORLD, MPI_ERR_ARG, 
                                      "MPI::Datatype::Create_keyval");
    }

    ret = ompi_attr_create_keyval(TYPE_ATTR, copy_fn, delete_fn,
                                  &keyval, extra_state, 0,
                                  cxx_type_keyval_destructor);
    if (OMPI_SUCCESS != ret) {
        return ret;
    }

    keyval_pair_t* copy_and_delete = 
        new keyval_pair_t(cxx_pair_copy, cxx_pair_delete);
    OPAL_THREAD_LOCK(MPI::mpi_map_mutex);
    MPI::Datatype::mpi_type_keyval_fn_map[keyval] = copy_and_delete;
    OPAL_THREAD_UNLOCK(MPI::mpi_map_mutex);
    return keyval;
}


// This function is called back out of the keyval destructor in the C
// layer when the keyval is not be used by any attributes anymore,
// anywhere.  So we can definitely safely remove the entry for this
// keyval from the C++ map.
static void cxx_type_keyval_destructor(int keyval) 
{
    OPAL_THREAD_LOCK(MPI::mpi_map_mutex);
    if (MPI::Datatype::mpi_type_keyval_fn_map.end() !=
        MPI::Datatype::mpi_type_keyval_fn_map.find(keyval) &&
        NULL != MPI::Datatype::mpi_type_keyval_fn_map[keyval]) {
        delete MPI::Datatype::mpi_type_keyval_fn_map[keyval];
        MPI::Datatype::mpi_type_keyval_fn_map.erase(keyval);
    }
    OPAL_THREAD_UNLOCK(MPI::mpi_map_mutex);
}

