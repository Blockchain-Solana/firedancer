$(call add-objs,fd_alloc,fd_util)
$(call add-hdrs,fd_alloc.h)
$(call make-bin,fd_alloc_ctl,fd_alloc_ctl,fd_util)
$(call make-unit-test,test_alloc,test_alloc,fd_util)
$(call add-test-scripts,test_alloc_ctl)

