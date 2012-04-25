#ifndef __CLUSTERING_ADMINISTRATION_NAMESPACE_INTERFACE_REPOSITORY_HPP__
#define __CLUSTERING_ADMINISTRATION_NAMESPACE_INTERFACE_REPOSITORY_HPP__

#include "errors.hpp"
#include <boost/ptr_container/ptr_map.hpp>

#include "clustering/immediate_consistency/query/namespace_interface.hpp"
#include "clustering/administration/namespace_metadata.hpp"

template <class protocol_t>
class namespace_repo_t {
public:
    namespace_repo_t(mailbox_manager_t *, 
                         boost::shared_ptr<semilattice_read_view_t<namespaces_semilattice_metadata_t<protocol_t> > >,
                         clone_ptr_t<watchable_t<std::map<peer_id_t, namespaces_directory_metadata_t<protocol_t> > > >);

    typedef std::map<master_id_t, master_business_card_t<protocol_t> > master_map_t;

    struct namepsace_if_access_t {
        namepsace_if_access_t()
            : namespace_if(NULL)
        { }
        namepsace_if_access_t(cluster_namespace_interface_t<protocol_t> *_namespace_if, auto_drainer_t::lock_t _lock) 
            : namespace_if(_namespace_if), lock(_lock)
        { }
        cluster_namespace_interface_t<protocol_t> *namespace_if;
    private:
        auto_drainer_t::lock_t lock;
    };

    namepsace_if_access_t get_namespace_if(namespace_id_t n_id);

private:
    mailbox_manager_t *mailbox_manager;
    boost::shared_ptr<semilattice_read_view_t<namespaces_semilattice_metadata_t<protocol_t> > > namespaces_semilattice_metadata;
    clone_ptr_t<watchable_t<std::map<peer_id_t, namespaces_directory_metadata_t<protocol_t> > > > namespaces_directory_metadata;

public:
    typedef boost::ptr_map<namespace_id_t, cluster_namespace_interface_t<protocol_t> > interface_map_t;
private:
    interface_map_t interface_map;
    auto_drainer_t drainer;
};

#include "clustering/administration/namespace_interface_repository.tcc"

#endif