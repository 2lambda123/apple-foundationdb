# Generate flow modulemap

function(generate_modulemap out module target)
  cmake_parse_arguments(ARG "" "" "OMIT" ${ARGN})

  set(MODULE_NAME "${module}")
  string(TOLOWER ${module} module_lower)
  get_target_property(MODULE_HEADERS ${target} HEADER_FILES)
  foreach(header ${MODULE_HEADERS})
    get_filename_component(fname ${header} NAME)
    if(NOT ${fname} IN_LIST ARG_OMIT)
      get_filename_component(headerdir ${header} DIRECTORY)
      get_filename_component(dirname ${headerdir} NAME)
      set(topdirname ${dirname})
      # FIXME: we need to account for headers in subdirectories.
      # THIS is a hack for single level directory.
      if (NOT ${dirname} MATCHES ${module_lower})
        get_filename_component(headerdir2 ${headerdir} DIRECTORY)
        get_filename_component(dirname2 ${headerdir2} NAME)
        set(topdirname ${dirname2})
        set(dirname "${dirname2}/${dirname}")
        if (NOT ${dirname2} MATCHES ${module_lower})
            get_filename_component(headerdir3 ${headerdir2} DIRECTORY)
            get_filename_component(dirname3 ${headerdir3} NAME)
            set(topdirname ${dirname3})
            set(dirname "${dirname3}/${dirname}")
        endif()
      endif()
      set(header_list "${header_list}    header \"${dirname}/${fname}\"\n")
      # needs 3.20
      #cmake_path(IS_PREFIX ${CMAKE_BINARY_DIR} ${headerdir} isGenerated)
      # if (NOT ${isGenerated})
      if (NOT ${headerdir} MATCHES ${CMAKE_BINARY_DIR})
        set(vfs_roots "${vfs_roots}  {\"type\": \"file\", \"name\": \"${CMAKE_BINARY_DIR}/${topdirname}/include/${dirname}/${fname}\", \"external-contents\": \"${header}\"},\n")
      endif()
    endif()
  endforeach()
  set(MODULE_HEADERS "${header_list}")
  configure_file("${CMAKE_SOURCE_DIR}/swifttestapp/empty.modulemap" "${out}/module.modulemap" @ONLY)
  set(VFS_ROOTS "${vfs_roots}")
  configure_file("${CMAKE_SOURCE_DIR}/swifttestapp/headeroverlay.yaml" "${out}/headeroverlay.yaml" @ONLY)
endfunction()
