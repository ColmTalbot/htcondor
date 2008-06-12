/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

/*

The classes here are all for communicating via message objects.  The
basic idea is to provide some structure so that details of how the
messages are delivered (e.g. asynchronous/synchronous) are easy to
change without rewriting lots of code.

To create a new type of message, derive a class from DCMsg.  Override
the virtual functions for reading/writing data to the stream.  If
necessary, override functions for handling events such as "message
sent" or "send failed".  For example, once the message is sent, you could
tell it to asynchronously receive a reply message.

To use a message class, instantiate it, set the callback function (if
desired), and send it, using Daemon::sendMsg().  Example:

	// Create startd daemon object and RELEASE_CLAIM message
	// using classy_counted_ptr, just to make it clear that
	// these objects will be garbage collected automatically.
	// In this example, we use a generic "string" message, but
	// in other cases, a specially defined message type may be needed.
	classy_counted_ptr<DCStartd> startd = new DCStartd( startd_addr );
	classy_counted_ptr<DCStringMsg> msg = new DCStringMsg(
	    RELEASE_CLAIM,
	    claim_id );

	// Log successful delivery at D_ALWAYS
	// (Failure is D_ALWAYS by default.)
	msg->setSuccessDebugLevel(D_ALWAYS);


	// If we need to be called back after successful/failed delivery,
	// register a callback function.
	msg->setCallback( new DCMsgCallback(
	    DCMsgCallback::CppFunction)&MyClass::MyCallbackFunction,
	    this ) );


	// Send the message to the startd asynchronously, using TCP.
	startd->sendMsg( msg.get(), Stream::reli_sock, STARTD_CONTACT_TIMEOUT );


	// To cancel a message that is waiting on asynchronous events, use
	// cancelMessage() on either the message or callback object.
*/


#ifndef DC_MESSAGE_H
#define DC_MESSAGE_H

/* We have circular references between DCMsg (declared in this file)
 * and Daemon (declared in daemon.h), so we need to foward declare
 * DCMsg before the includes.
 */
class DCMessenger;
class DCMsg;
class DCMsgCallback;

#include "daemon.h"
#include "classy_counted_ptr.h"
#include "dc_service.h"


/*
  This is a base class for sending CEDAR commands.  By default, the
  caller of this class is not blocked if a TCP connection is required
  to create a security session.
*/

class DCMsg: public ClassyCountedPtr {
public:
	DCMsg(int cmd);
	virtual ~DCMsg();

	enum MessageClosureEnum {
		MESSAGE_FINISHED,  // tells DCMessenger that sock may be closed
		MESSAGE_CONTINUING // tells DCMessenger not to close sock
	};

	enum DeliveryStatus {
		DELIVERY_PENDING,
		DELIVERY_SUCCEEDED,
		DELIVERY_FAILED,
		DELIVERY_CANCELED
	};

		/* sets the callback function to call when MESSAGE_FINISHED
		   is returned by one of the message closure functions */
	void setCallback(classy_counted_ptr<DCMsgCallback> cb);

	void doCallback();

		/* writeMsg() is called by DCMessenger once sock is ready to
		   receive data.
		   returns: true on success; false on failure */
	virtual bool writeMsg( DCMessenger *messenger, Sock *sock ) = 0;

		/* readMsg() is called by DCMessenger once sock is ready to
		   receive data.
		   returns: true on success; false on failure */
	virtual bool readMsg( DCMessenger *messenger, Sock *sock ) = 0;

		/* messageSent() is called by DCMessenger upon successful
		   delivery of message
		   return: MESSAGE_FINISHED if sock may now be closed
		           MESSAGE_CONTINUING otherwise */
	virtual MessageClosureEnum messageSent(
				DCMessenger *messenger, Sock *sock );

		/* messageReceived() is called by DCMessenger upon successful
		   delivery of message
		   return: MESSAGE_FINISHED if sock may now be closed
		           MESSAGE_CONTINUING otherwise */
	virtual MessageClosureEnum messageReceived(
				DCMessenger *messenger, Sock *sock );

		/* messageSendFailed() is called by DCMessenger when message
		   delivery failed; details are in errstack. */
	virtual void messageSendFailed( DCMessenger *messenger );

		/* messageReceiveFailed() is called by DCMessenger when message
		   delivery failed; details are in errstack. */
	virtual void messageReceiveFailed( DCMessenger *messenger );

		/* reportSuccess() logs successful delivery of message */
	virtual void reportSuccess( DCMessenger *messenger );

		/* reportFailure() logs failure to deliver message */
	virtual void reportFailure( DCMessenger *messenger );

		/* descriptive name of this type of message */
	virtual char const *name();

		/* override default debug level (D_FULLDEBUG) */
	void setSuccessDebugLevel(int level) {m_msg_success_debug_level = level;}

		/* override default debug level (D_ALWAYS|D_FAILURE) */
	void setFailureDebugLevel(int level) {m_msg_failure_debug_level = level;}

	int successDebugLevel() {return m_msg_success_debug_level;}

	int failureDebugLevel() {return m_msg_failure_debug_level;}

		/* add an error message to the error stack */
	void addError( int code, char const *format, ... );

		/* this calls addError() after a failed call to sock->put()/get() */
	void sockFailed( Sock *sock );

		/* returns indicator of whether msg delivery succeeded or not */
	DeliveryStatus deliveryStatus() {return m_delivery_status;}

		/* sets msg delivery status */
	void deliveryStatus(DeliveryStatus s);

		/* Stop any pending operations related to this message.
		   The callback (if any) will still be called.  The delivery status
		   will be set to DELIVERY_CANCELED.
		*/
	virtual void cancelMessage();

	friend class DCMessenger;
private:
	int m_cmd;
	classy_counted_ptr<DCMsgCallback> m_cb;
	int m_msg_success_debug_level;
	int m_msg_failure_debug_level;
	CondorError m_errstack;
	DeliveryStatus m_delivery_status;
	classy_counted_ptr<DCMessenger> m_messenger;

	void connectFailure( DCMessenger *messenger );

	void callMessageSendFailed( DCMessenger *messenger );
	void callMessageReceiveFailed( DCMessenger *messenger );
	MessageClosureEnum callMessageSent(
				DCMessenger *messenger, Sock *sock );
	MessageClosureEnum callMessageReceived(
				DCMessenger *messenger, Sock *sock );

	void setMessenger( DCMessenger *messenger );
};

/*
 DCMessenger - a class for managing sending/receiving DCMsg objects
               over a socket either synchronously, or asynchronously.
               An instance of DCMessenger is used internally by
               Daemon::sendMsg(), so the code that deals at a higher
               layer with messages may never need to be aware of this
               class.
 */

class DCMessenger: public ClassyCountedPtr, public Service {
public:
		// This constructor is intended for use on the sending side,
		// where the peer is represented by a Daemon object.
		// Daemon should (in almost all cases) be allocated on the heap,
		// to ensure that it lasts for the lifetime of this DCMessenger
		// object.  Garbage collection is handled via classy_counted_ptr.
	DCMessenger( classy_counted_ptr<Daemon> daemon );

		// This constructor is intended for use on the receiving end
		// of a connection, where the peer is represented by an
		// existing sock, rather than a Daemon object.  This class
		// assumes ownership of sock and will delete it when the class
		// is destroyed.
	DCMessenger( Sock *sock );

	~DCMessenger();

		// Start a command, doing a non-blocking connection if necessary.
		// This operation calls inc/decRefCount() to manage garbage collection
		// of this messenger object as well as the message object.
	void startCommand( classy_counted_ptr<DCMsg> msg, Stream::stream_type st = Stream::reli_sock, int timeout = 0 );

		// Like startCommand(), except set a timer for delay seconds
		// before starting the command.
	void startCommandAfterDelay( unsigned int delay, classy_counted_ptr<DCMsg> msg, Stream::stream_type st = Stream::reli_sock, int timeout = 0 );

		// Send a message from beginning to end, right now.  By the time
		// this command returns, the message delivery status should be
		// set to success/failure, and the message delivery hooks will
		// have been called.
	void sendBlockingMsg( classy_counted_ptr<DCMsg> msg, Stream::stream_type st = Stream::reli_sock, int timeout = 0 );

		// Registers this messenger to receive notice when a message arrives
		// on this socket and then call msg->readMsg() when one does.
		// This operation calls inc/decRefCount() to manage garbage collection
		// of this messenger object as well as the message object.
		// This class also assumes ownership of sock and will delete it
		// when finished with the operation.
	void startReceiveMsg( classy_counted_ptr<DCMsg> msg, Sock *sock );

		// Write a message to a socket that is ready to receive it.
		// If you are starting a new command, simply call startCommand()
		// instead of calling this function directly.
	void writeMsg( classy_counted_ptr<DCMsg> msg, Sock *sock );

		// Read a message from a socket that is ready to be read.
		// Call startReceiveMsg() to initiate a non-blocking read
		// which will eventually call this function.
	void readMsg( classy_counted_ptr<DCMsg> msg, Sock *sock );

		// Returns information about who we are talking to.
	char const *peerDescription();

	friend class DCMsg;
private:
		// This is called by DaemonClient when startCommand has finished.
	static void connectCallback(bool success, Sock *sock, CondorError *errstack, void *misc_data);

		// This is called by DaemonCore when the sock has data.
	int receiveMsgCallback(Stream *sock);

		// This is called by DaemonCore when the delay time has expired.
	int startCommandAfterDelay_alarm();

		// Delete a sock unless it happens to be m_sock.
	void doneWithSock(Stream *sock);

		// Cancel a non-blocking operation such as startCommand()
		// or startReceiveMsg().  The appropriate failure callback
		// for the current operation will be called:
		// messageSendFailed() or messageReceiveFailed().
	void cancelMessage( classy_counted_ptr<DCMsg> msg );

	classy_counted_ptr<Daemon> m_daemon; // our daemon client object (if any)
	Sock *m_sock;         // otherwise, we will just have a socket

	classy_counted_ptr<DCMsg> m_callback_msg; // The current message waiting for a callback.
	Sock *m_callback_sock; // The current sock waiting for a callback.
	enum pending_operation_enum {
		NOTHING_PENDING,
		START_COMMAND_PENDING,
		RECEIVE_MSG_PENDING
	} m_pending_operation;
};


/*
  DCMsgCallback - a class for registering a function to be called when
  a message has been delivered.
 */

class DCMsgCallback: public ClassyCountedPtr {
 public:
	typedef void (Service::*CppFunction)(DCMsgCallback *cb);

		// As needed, additional constructors should be added to handle
		// other types of callback functions.
	DCMsgCallback(CppFunction fn,Service *service,void *misc_data=NULL);

	virtual void doCallback();

	void setMiscDataPtr(void *misc_data) {m_misc_data = misc_data;}
	void *getMiscDataPtr() {return m_misc_data;}

	DCMsg *getMessage() {return m_msg.get();}

		/* Call the message's cancelMessage() function.
		   The callback (if any) will still be called.  The delivery status
		   will be set to DELIVERY_CANCELED.
		*/
	void cancelMessage();

	friend class DCMsg;
 private:
	CppFunction m_fn_cpp;
	Service *m_service;
	void *m_misc_data;
	classy_counted_ptr<DCMsg> m_msg;

		// This is called by DCMsg::setMessage().
	void setMessage(classy_counted_ptr<DCMsg> msg) {m_msg = msg;}
};


/*
 * DCStringMsg is a simple message consisting of one string.
 */

class DCStringMsg: public DCMsg {
public:
	DCStringMsg( int cmd, char const *str );

	bool writeMsg( DCMessenger *messenger, Sock *sock );
	bool readMsg( DCMessenger *messenger, Sock *sock );

private:
	MyString m_str;
};


#endif
