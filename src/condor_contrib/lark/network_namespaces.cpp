
#include <net/if.h>

#include "condor_common.h"
#include "condor_config.h"
#include "condor_arglist.h"
#include "my_popen.h"

#include "network_namespaces.h"
#include "network_manipulation.h"

static NetworkNamespaceManager instance;

NetworkNamespaceManager::NetworkNamespaceManager() :
	m_state(UNCREATED), m_network_namespace(""),
	m_internal_pipe(""), m_external_pipe(""),
	m_sock(-1), m_created_pipe(false),
	m_iplock_external(NULL),
	m_iplock_internal(NULL)
	{
		//PluginManager<NetworkManager>::registerPlugin(this);
		dprintf(D_FULLDEBUG, "Initialized a NetworkNamespaceManager plugin.\n");
	}

int NetworkNamespaceManager::PrepareNetwork(const std::string &uniq_namespace, const classad::ClassAd &job_ad, classad::ClassAd &machine_ad) {
	if (m_state != UNCREATED) {
		dprintf(D_FULLDEBUG, "Internal bug: NetworkNamespaceManager::CreateNamespace has already been invoked.\n");
		m_state = FAILED;
		return 1;
	}
	m_network_namespace = uniq_namespace;
	m_internal_pipe = "i_" + m_network_namespace;
	m_external_pipe = "e_" + m_network_namespace;

	if ((m_sock = create_socket()) < 0) {
		dprintf(D_ALWAYS, "Unable to create a socket to talk to the kernel for network namespaces.\n");
		m_state = FAILED;
		return 1;
	}

	const char * namespace_script;
	int rc = 0;

	namespace_script = param("NETWORK_NAMESPACE_CREATE_SCRIPT");
	if (!namespace_script)
	{
		dprintf(D_ALWAYS, "Parameter NETWORK_NAMESPACE_CREATE_SCRIPT is not specified.\n");
		m_state = FAILED;
		return 1;
	}

	if ((rc = CreateNetworkPipe())) {
		dprintf(D_ALWAYS, "Unable to create a new set of network pipes; cannot create a namespace.\n");
		m_state = FAILED;
		return rc;
	}

	// Grab an IP address for the external interface.
	std::string network_spec;
	if (!param(network_spec, "NAT_NETWORK", "192.168.181.0/255.255.255.0"))
	{
		dprintf(D_FULLDEBUG, "Parameter NAT_NETWORK is not specified; using default %s.\n", network_spec.c_str());
	}
	m_iplock_external.reset(new IPLock(network_spec));
	m_iplock_internal.reset(new IPLock(network_spec));
	if (!m_iplock_external->Lock(m_external_address))
	{
		dprintf(D_ALWAYS, "Unable to lock an external IP address to use.\n");
		m_state = FAILED;
		return 1;
	}
	if (!m_iplock_internal->Lock(m_internal_address))
	{
		dprintf(D_ALWAYS, "Unable to lock an internal IP address for use.\n");
		m_state = FAILED;
		return 1;
	}
	dprintf(D_FULLDEBUG, "Using address %s for external portion of NAT.\n", m_external_address.to_ip_string().Value());
	dprintf(D_FULLDEBUG, "Using address %s for internal portion of NAT.\n", m_internal_address.to_ip_string().Value());
	m_internal_address_str = m_internal_address.to_ip_string();

	ArgList args;
	std::string external_address = m_external_address.to_ip_string();
	args.AppendArg(namespace_script);
	args.AppendArg(external_address.c_str());
	args.AppendArg(m_internal_address_str.c_str());
	args.AppendArg(m_network_namespace);
	args.AppendArg(m_external_pipe);
	dprintf(D_FULLDEBUG, "NetworkNamespaceManager nat setup: %s %s %s %s\n", namespace_script, external_address.c_str(), m_network_namespace.c_str(), m_external_pipe.c_str());

	FILE *fp = my_popen(args, "r", TRUE);
	if (fp == NULL) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager::CreateNamespace: my_popen failure on %s: (errno=%d) %s\n", args.GetArg(0), errno, strerror(errno));
		m_state = FAILED;
		return 1;
	}

	char out_buff[1024];
	while (fgets(out_buff, 1024, fp) != NULL)
		dprintf(D_FULLDEBUG, "NetworkNamespaceManager::CreateNamespace: NETWORK_NAMESPACE_CREATE_SCRIPT output: %s", out_buff);
	int status = my_pclose(fp);
	if (status == -1) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager::CreateNamespace: Error when running NETWORK_NAMESPACE_CREATE_SCRIPT (errno=%d) %s\n", errno, strerror(errno));
	} else {
		if (WIFEXITED(status)) {
			if ((status = WEXITSTATUS(status))) {
				dprintf(D_ALWAYS, "NetworkNamespaceManager::CreateNamespace NETWORK_NAMESPACE_CREATE_SCRIPT exited with status %d\n", status);
				m_state = FAILED;
				return 1;
			} else {
				dprintf(D_FULLDEBUG, "NetworkNamespaceManager::CreateNamespace NETWORK_NAMESPACE_CREATE_SCRIPT was successful.\n");
			}
		} else if (WIFSIGNALED(status)) {
			status = WTERMSIG(status);
			dprintf(D_ALWAYS, "NetworkNamespaceManager::CreateNamespace NETWORK_NAMESPACE_CREATE_SCRIPT terminated with signal %d\n", status);
			m_state = FAILED;
			return 1;
		}
	}

	m_state = CREATED;
	return 0;
}

int NetworkNamespaceManager::CreateNetworkPipe() {
	int rc;

	m_internal_pipe = "i_" + m_network_namespace;
        if ((rc = create_veth(m_sock, m_external_pipe.c_str(), m_internal_pipe.c_str()))) {
                dprintf(D_ALWAYS, "Failed to create veth devices %s/%s.\n", m_external_pipe.c_str(), m_internal_pipe.c_str());
                m_state = FAILED;
		return rc;
        }
	m_created_pipe = true;

	dprintf(D_FULLDEBUG, "Created a pair of veth devices (%s, %s).\n", m_external_pipe.c_str(), m_internal_pipe.c_str());
        
	return 0;
}

int NetworkNamespaceManager::PreFork() {
	if (m_state == UNCREATED)
		return 0;

	if ((pipe(m_p2c) < 0) || (pipe(m_c2p) < 0)) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager: pipe() failed with %s (errno=%d).\n", strerror(errno), errno);
		return -1;
	}
	if ((fcntl(m_p2c[0], F_SETFD, FD_CLOEXEC) < 0) || (fcntl(m_c2p[1], F_SETFD, FD_CLOEXEC) < 0)) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager: fcntl() failed with %s (errno=%d).\n", strerror(errno), errno);
		return -1;
	}
	return 0;
}

int NetworkNamespaceManager::PostForkChild() {
	if (m_state == UNCREATED)
		return 0;

	// Close the end of the pipes that aren't ours
	close(m_p2c[1]);
	close(m_c2p[0]);

	// Call unshare, and tell the parent to proceed.
	// If there's an error, simply return non-zero; the parent will get an EPIPE.
	int rc, rc2;

	rc = ::unshare(CLONE_NEWNET);
	if (rc) {
		dprintf(D_ALWAYS, "Failed to unshare the network namespace\n");
		return errno;
	}

	while (((rc2 = write(m_c2p[1], &rc, sizeof(rc))) < 0) && (errno == EINTR)) {}
	if (rc2 < 0) {
		dprintf(D_ALWAYS, "Error communicating with child: %s (errno=%d).\n", strerror(errno), errno);
		return errno;
	}


	// Wait until the parent indicates it's OK to proceed
	while (((rc2 = read(m_p2c[0], &rc, sizeof(rc))) < 0) && (errno == EINTR)) {}
	if (rc2 == -1) {
		dprintf(D_ALWAYS, "Error reading from parent: %s (errno=%d).\n", strerror(errno), errno);
		return errno;
	}
	if (rc != 0) {
		dprintf(D_ALWAYS, "Got an error from the parent: %d.\n", rc);
		return rc;
	}

        //
	// Manipulate our network configuration in the child.
	// Notice that we open a new socket to the kernel - this is because the
	// other socket still talks to the original namespace.
	//
	// Note: Because we may be in a shared-memory clone, do NOT modify the heap.
	// This is why we saved the IPv4 address in m_internal_address_str instead of just
	// recreating it from m_internal_address
	int sock;
	dprintf(D_FULLDEBUG, "Child proceeding to configure networking for address %s.\n", m_internal_address_str.c_str());
	if ((sock = create_socket()) < 0) {
		dprintf(D_ALWAYS, "Unable to create socket to talk to kernel for child.\n");
		rc = 1;
		goto failed_socket;
	}
	if (add_address(sock, m_internal_address_str.c_str(), m_internal_pipe.c_str())) {
		dprintf(D_ALWAYS, "Unable to add address %s to %s.\n", m_internal_address_str.c_str(), m_internal_pipe.c_str());
		rc = 2;
		goto finalize_child;
	}
	if (set_status(sock, m_internal_pipe.c_str(), IFF_UP)) {
		dprintf(D_ALWAYS, "Unable to bring up interface %s.\n", m_internal_pipe.c_str());
		rc = 3;
		goto finalize_child;
	}
	/*if (add_local_route(sock, m_internal_address_str.c_str(), m_internal_pipe.c_str(), 24)) {
		dprintf(D_ALWAYS, "Unable to add local route via %s\n", m_internal_address_str.c_str());
		rc = 4;
		goto finalize_child;
	}*/
	if (add_default_route(sock, m_external_address.to_ip_string().Value())) {
		dprintf(D_ALWAYS, "Unable to add default route via %s\n", m_external_address.to_ip_string().Value());
		rc = 5;
		goto finalize_child;
	}

	m_state = INTERNAL_CONFIGURED;

	// Note we don't write anything to the parent, on success or failure.
	// This is because the parent must wait until the child exits, and the fact
	// m_c2p[1] is marked FD_CLOEXEC will cause an EPIPE to be recieved by the parent,
	// the sign of success.
	// The "normal" Create_Process communication pipe takes care of the error case.

finalize_child:
	close(sock);
failed_socket:
	return rc;

}

int NetworkNamespaceManager::PostForkParent(pid_t pid) {
	if (m_state == UNCREATED)
		return 0;

        // Close the end of the pipes that aren't ours
	close(m_p2c[0]);
	close(m_c2p[1]);

	// Wait for the child to indicate we can proceed (child must call unshare())
	int rc2, rc=0;
	while (((rc2 = read(m_c2p[0], &rc, sizeof(rc))) < 0) && (errno == EINTR)) {}
	if (rc2 == -1) {
		if (errno == EPIPE) {
			// No log: the normal error-fighting mechanisms will log something.
			return 1;
		} else  {
			dprintf(D_ALWAYS, "Error reading from child fd %d: %s (errno=%d).\n", m_c2p[0], strerror(errno), errno);
			return errno;
		}
	}
	if (rc != 0) {
		dprintf(D_ALWAYS, "Got an error from the child: %d.\n", rc);
		return rc;
	}

	if (m_state != CREATED) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager in incorrect state %d to send device to internal namespace\n", m_state);
		m_state = FAILED;
		return 1;
	}
	
	if ((rc = set_netns(m_sock, m_internal_pipe.c_str(), pid))) {
		dprintf(D_ALWAYS, "Failed to send %s to network namespace %d.\n", m_internal_pipe.c_str(), pid);
	}

	m_state = PASSED;

	// Advance automatically, in case we didn't use clone.
	m_state = INTERNAL_CONFIGURED;

	// Inform the child it can advance.
	if (rc == 0) {
		while (((rc2 = write(m_p2c[1], &rc, sizeof(rc))) < 0) && (errno == EINTR)) {}
		if (rc2 < 0) {
			dprintf(D_ALWAYS, "Error communicating with child: %s (errno=%d).\n", strerror(errno), errno);
			rc = rc2;
		}
	}

	// Wait until the child's exec or error.
	if (rc == 0) {
		while (((rc2 = read(m_c2p[0], &rc, sizeof(rc))) < 0) && (errno == EINTR)) {}
		if (rc2 > 0) {
			dprintf(D_ALWAYS, "Got error code from child: %d\n", rc);
		} else if ((errno) && (errno != EPIPE)) {
			dprintf(D_ALWAYS, "Error reading from child: %s (errno=%d).\n", strerror(errno), errno);
			rc = errno;
		}
	}

	return rc;

}

int NetworkNamespaceManager::PerformJobAccounting(classad::ClassAd *classad) {
	if (m_state == UNCREATED)
		return 0;

	int rc = 0;
	if (m_state == INTERNAL_CONFIGURED) {
		dprintf(D_FULLDEBUG, "Polling netfilter for network statistics\n");
		rc = perform_accounting(m_network_namespace.c_str(), JobAccountingCallback, (void *)&m_statistics);
	}
	if (classad) {
		classad->Update(m_statistics);
	}
	return rc;
}

int NetworkNamespaceManager::JobAccountingCallback(const unsigned char * rule_name, long long bytes, void * callback_data) {
	classad::ClassAd &classad = *(classad::ClassAd*)callback_data;
	std::string attr_name("Network");
	attr_name.append((const char *)rule_name);
	classad.InsertAttr(attr_name, double(bytes), classad::Value::B_FACTOR);
	dprintf(D_FULLDEBUG, "Network accounting: %s = %lld\n", attr_name.c_str(), bytes);
	//classad.Assign(attr_name, bytes);
	return 0;
}

int NetworkNamespaceManager::Cleanup(const std::string &) {

	// Try to only clean once.
	if (m_state == CLEANED) {
		dprintf(D_ALWAYS, "Called Cleanup on an already-cleaned NetworkNamespaceManager!\n");
		return 1;
	}

	if (m_state == UNCREATED) {
		// We never created the namespace.  Do nothing.
		return 0;
	}

	if (!m_created_pipe) {
		// Not much to do in this case.
		return 0;
	}

	int rc2;
	rc2 = RunCleanupScript();

	if (m_sock < 0) {
		dprintf(D_ALWAYS, "Unable to delete device as socket is invalid.\n");
		return 1;
	}

	int rc = 0, rc3;
	// If the job launched successfully, the pipe may get deleted by the OS
	// automatically.  In such a case, the delete_veth will return with
	// either "no such device" or "invalid value".  Not sure why the kernel
	// gives both - maybe some sort of race?
	if ((rc3 = delete_veth(m_sock, m_external_pipe.c_str()))  && (rc3 != ENODEV) && (rc3 != EINVAL)) {
		dprintf(D_ALWAYS, "Failed to delete the veth interface; rc=%d\n", rc);
		rc = rc3;
	}

	m_iplock_external.reset(NULL);
	m_iplock_internal.reset(NULL);

	return rc2 ? rc2 : rc;
}

int NetworkNamespaceManager::RunCleanupScript() {
	const char * namespace_script = param("NETWORK_NAMESPACE_DELETE_SCRIPT");

	ArgList args;
	args.AppendArg(namespace_script);
	args.AppendArg(m_network_namespace);
	args.AppendArg(m_external_pipe);
	args.AppendArg(m_internal_address_str);

	FILE *fp = my_popen(args, "r", TRUE);
	if (fp == NULL) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager::Cleanup : "
			"my_popen failure on %s: (errno=%d) %s\n",
			 args.GetArg(0), errno, strerror(errno));
		m_state = FAILED;
		return 1;
	}

	char out_buff[1024];
	while (fgets(out_buff, 1024, fp) != NULL)
		dprintf(D_FULLDEBUG, "NetworkNamespaceManager::Cleanup: NETWORK_NAMESPACE_DELETE_SCRIPT output: %s", out_buff);
	int status = my_pclose(fp);
	if (status == -1) {
		dprintf(D_ALWAYS, "NetworkNamespaceManager::Cleanup: Error when running NETWORK_NAMESPACE_DELETE_SCRIPT (errno=%d) %s\n", errno, strerror(errno));
	} else {
		if (WIFEXITED(status)) {
			if ((status = WEXITSTATUS(status))) {
				dprintf(D_ALWAYS, "NetworkNamespaceManager::Cleanup NETWORK_NAMESPACE_DELETE_SCRIPT exited with status %d\n", status);
			} else {
				dprintf(D_FULLDEBUG, "NetworkNamespaceManager::Cleanup NETWORK_NAMESPACE_DELETE_SCRIPT was successful.\n");
			}
		} else if (WIFSIGNALED(status)) {
			status = WTERMSIG(status);
			dprintf(D_ALWAYS, "NetworkNamespaceManager::Cleanup NETWORK_NAMESPACE_DELETE_SCRIPT terminated with signal %d\n", status);
		}
	}

	return 0;
}

