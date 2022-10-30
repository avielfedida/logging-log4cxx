/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(_MSC_VER)
	#pragma warning ( disable: 4231 4251 4275 4786 )
#endif

#include <log4cxx/rolling/rollingfileappender.h>
#include <log4cxx/helpers/loglog.h>
#include <log4cxx/rolling/rolloverdescription.h>
#include <log4cxx/helpers/fileoutputstream.h>
#include <log4cxx/helpers/bytebuffer.h>
#include <log4cxx/rolling/fixedwindowrollingpolicy.h>
#include <log4cxx/rolling/manualtriggeringpolicy.h>
#include <log4cxx/helpers/transcoder.h>
#include <log4cxx/private/fileappender_priv.h>
#include <mutex>

using namespace log4cxx;
using namespace log4cxx::rolling;
using namespace log4cxx::helpers;
using namespace log4cxx::spi;

struct RollingFileAppender::RollingFileAppenderPriv : public FileAppenderPriv
{
	RollingFileAppenderPriv() :
		FileAppenderPriv(),
		fileLength(0) {}

	/**
	 * Triggering policy.
	 */
	TriggeringPolicyPtr triggeringPolicy;

	/**
	 * Rolling policy.
	 */
	RollingPolicyPtr rollingPolicy;

	/**
	 * Length of current active log file.
	 */
	size_t fileLength;

	/**
	 *  save the loggingevent
	 */
	spi::LoggingEventPtr _event;
};

#define _priv static_cast<RollingFileAppenderPriv*>(m_priv.get())

IMPLEMENT_LOG4CXX_OBJECT(RollingFileAppender)


/**
 * Construct a new instance.
 */
RollingFileAppender::RollingFileAppender() :
	FileAppender (std::make_unique<RollingFileAppenderPriv>())
{
}

/**
 * Prepare instance of use.
 */
void RollingFileAppender::activateOptions(Pool& p)
{
	if (_priv->rollingPolicy == NULL)
	{
		auto fwrp = std::make_shared<FixedWindowRollingPolicy>();
		fwrp->setFileNamePattern(getFile() + LOG4CXX_STR(".%i"));
		_priv->rollingPolicy = fwrp;
	}

	//
	//  if no explicit triggering policy and rolling policy is both.
	//
	if (_priv->triggeringPolicy == NULL)
	{
		TriggeringPolicyPtr trig = log4cxx::cast<TriggeringPolicy>(_priv->rollingPolicy);

		if (trig != NULL)
		{
			_priv->triggeringPolicy = trig;
		}
	}

	if (_priv->triggeringPolicy == NULL)
	{
		_priv->triggeringPolicy = std::make_shared<ManualTriggeringPolicy>();
	}

	{
		std::lock_guard<std::recursive_mutex> lock(_priv->mutex);
		_priv->triggeringPolicy->activateOptions(p);
		_priv->rollingPolicy->activateOptions(p);

		try
		{
			RolloverDescriptionPtr rollover1 =
				_priv->rollingPolicy->initialize(getFile(), getAppend(), p);

			if (rollover1 != NULL)
			{
				ActionPtr syncAction(rollover1->getSynchronous());

				if (syncAction != NULL)
				{
					syncAction->execute(p);
				}

				_priv->fileName = rollover1->getActiveFileName();
				_priv->fileAppend = rollover1->getAppend();

				//
				//  async action not yet implemented
				//
				ActionPtr asyncAction(rollover1->getAsynchronous());

				if (asyncAction != NULL)
				{
					asyncAction->execute(p);
				}
			}

			File activeFile;
			activeFile.setPath(getFile());

			if (getAppend())
			{
				_priv->fileLength = activeFile.length(p);
			}
			else
			{
				_priv->fileLength = 0;
			}

			FileAppender::activateOptionsInternal(p);
		}
		catch (std::exception&)
		{
			LogLog::warn(
				LogString(LOG4CXX_STR("Exception will initializing RollingFileAppender named "))
				+ getName());
		}
	}
}

/**
   Implements the usual roll over behaviour.

   <p>If <code>MaxBackupIndex</code> is positive, then files
   {<code>File.1</code>, ..., <code>File.MaxBackupIndex -1</code>}
   are renamed to {<code>File.2</code>, ...,
   <code>File.MaxBackupIndex</code>}. Moreover, <code>File</code> is
   renamed <code>File.1</code> and closed. A new <code>File</code> is
   created to receive further log output.

   <p>If <code>MaxBackupIndex</code> is equal to zero, then the
   <code>File</code> is truncated with no backup files created.

 * @return true if rollover performed.
 */
bool RollingFileAppender::rollover(Pool& p)
{
	std::lock_guard<std::recursive_mutex> lock(_priv->mutex);
	return rolloverInternal(p);
}

bool RollingFileAppender::rolloverInternal(Pool& p)
{
	//
	//   can't roll without a policy
	//
	if (_priv->rollingPolicy != NULL)
	{

		{
				try
				{
					RolloverDescriptionPtr rollover1(_priv->rollingPolicy->rollover(this->getFile(), this->getAppend(), p));

					if (rollover1 != NULL)
					{
						if (rollover1->getActiveFileName() == getFile())
						{
							closeWriter();

							bool success = true;

							if (rollover1->getSynchronous() != NULL)
							{
								success = false;

								try
								{
									success = rollover1->getSynchronous()->execute(p);
								}
								catch (std::exception& ex)
								{
									LogLog::warn(LOG4CXX_STR("Exception on rollover"));
									LogString exmsg;
									log4cxx::helpers::Transcoder::decode(ex.what(), exmsg);
									_priv->errorHandler->error(exmsg, ex, 0);
								}
							}

							if (success)
							{
								if (rollover1->getAppend())
								{
									_priv->fileLength = File().setPath(rollover1->getActiveFileName()).length(p);
								}
								else
								{
									_priv->fileLength = 0;
								}

								//
								//  async action not yet implemented
								//
								ActionPtr asyncAction(rollover1->getAsynchronous());

								if (asyncAction != NULL)
								{
									asyncAction->execute(p);
								}

								setFileInternal(
									rollover1->getActiveFileName(), rollover1->getAppend(),
									_priv->bufferedIO, _priv->bufferSize, p);
							}
							else
							{
								setFileInternal(
									rollover1->getActiveFileName(), true, _priv->bufferedIO, _priv->bufferSize, p);
							}
						}
						else
						{
							closeWriter();
							setFileInternal(rollover1->getActiveFileName());
							// Call activateOptions to create any intermediate directories(if required)
							FileAppender::activateOptionsInternal(p);
							OutputStreamPtr os(new FileOutputStream(
									rollover1->getActiveFileName(), rollover1->getAppend()));
							WriterPtr newWriter(createWriter(os));
							setWriterInternal(newWriter);

							bool success = true;

							if (rollover1->getSynchronous() != NULL)
							{
								success = false;

								try
								{
									success = rollover1->getSynchronous()->execute(p);
								}
								catch (std::exception& ex)
								{
									LogLog::warn(LOG4CXX_STR("Exception during rollover"));
									LogString exmsg;
									log4cxx::helpers::Transcoder::decode(ex.what(), exmsg);
									_priv->errorHandler->error(exmsg, ex, 0);
								}
							}

							if (success)
							{
								if (rollover1->getAppend())
								{
									_priv->fileLength = File().setPath(rollover1->getActiveFileName()).length(p);
								}
								else
								{
									_priv->fileLength = 0;
								}

								//
								//   async action not yet implemented
								//
								ActionPtr asyncAction(rollover1->getAsynchronous());

								if (asyncAction != NULL)
								{
									asyncAction->execute(p);
								}
							}

							writeHeader(p);
						}
						return true;
					}
				}
				catch (std::exception& ex)
				{
					LogLog::warn(LOG4CXX_STR("Exception during rollover"));
					LogString exmsg;
					log4cxx::helpers::Transcoder::decode(ex.what(), exmsg);
					_priv->errorHandler->error(exmsg, ex, 0);
				}
		}
	}

	return false;
}

/**
 * {@inheritDoc}
*/
void RollingFileAppender::subAppend(const LoggingEventPtr& event, Pool& p)
{
	// The rollover check must precede actual writing. This is the
	// only correct behavior for time driven triggers.
	if (
		_priv->triggeringPolicy->isTriggeringEvent(
			this, event, getFile(), getFileLength()))
	{
		//
		//   wrap rollover request in try block since
		//    rollover may fail in case read access to directory
		//    is not provided.  However appender should still be in good
		//     condition and the append should still happen.
		try
		{
			_priv->_event = event;
			rolloverInternal(p);
		}
		catch (std::exception& ex)
		{
			LogLog::warn(LOG4CXX_STR("Exception during rollover attempt."));
			LogString exmsg;
			log4cxx::helpers::Transcoder::decode(ex.what(), exmsg);
			_priv->errorHandler->error(exmsg);
		}
	}

	FileAppender::subAppend(event, p);
}

/**
 * Get rolling policy.
 * @return rolling policy.
 */
RollingPolicyPtr RollingFileAppender::getRollingPolicy() const
{
	return _priv->rollingPolicy;
}

/**
 * Get triggering policy.
 * @return triggering policy.
 */
TriggeringPolicyPtr RollingFileAppender::getTriggeringPolicy() const
{
	return _priv->triggeringPolicy;
}

/**
 * Sets the rolling policy.
 * @param policy rolling policy.
 */
void RollingFileAppender::setRollingPolicy(const RollingPolicyPtr& policy)
{
	_priv->rollingPolicy = policy;
}

/**
 * Set triggering policy.
 * @param policy triggering policy.
 */
void RollingFileAppender::setTriggeringPolicy(const TriggeringPolicyPtr& policy)
{
	_priv->triggeringPolicy = policy;
}

/**
 * Close appender.  Waits for any asynchronous file compression actions to be completed.
 */
void RollingFileAppender::close()
{
	FileAppender::close();
}

namespace log4cxx
{
namespace rolling
{
/**
 * Wrapper for OutputStream that will report all write
 * operations back to this class for file length calculations.
 */
class CountingOutputStream : public OutputStream
{
		/**
		 * Wrapped output stream.
		 */
	private:
		OutputStreamPtr os;

		/**
		 * Rolling file appender to inform of stream writes.
		 */
		RollingFileAppender* rfa;

	public:
		/**
		 * Constructor.
		 * @param os output stream to wrap.
		 * @param rfa rolling file appender to inform.
		 */
		CountingOutputStream(
			OutputStreamPtr& os1, RollingFileAppender* rfa1) :
			os(os1), rfa(rfa1)
		{
		}

		/**
		 * {@inheritDoc}
		 */
		void close(Pool& p)
		{
			os->close(p);
			rfa = 0;
		}

		/**
		 * {@inheritDoc}
		 */
		void flush(Pool& p)
		{
			os->flush(p);
		}

		/**
		 * {@inheritDoc}
		 */
		void write(ByteBuffer& buf, Pool& p)
		{
			os->write(buf, p);

			if (rfa != 0)
			{
				rfa->incrementFileLength(buf.limit());
			}
		}
};
}
}

/**
   Returns an OutputStreamWriter when passed an OutputStream.  The
   encoding used will depend on the value of the
   <code>encoding</code> property.  If the encoding value is
   specified incorrectly the writer will be opened using the default
   system encoding (an error message will be printed to the loglog.
 @param os output stream, may not be null.
 @return new writer.
 */
WriterPtr RollingFileAppender::createWriter(OutputStreamPtr& os)
{
	OutputStreamPtr cos = std::make_shared<CountingOutputStream>(os, this);
	return FileAppender::createWriter(cos);
}

/**
 * Get byte length of current active log file.
 * @return byte length of current active log file.
 */
size_t RollingFileAppender::getFileLength() const
{
	return _priv->fileLength;
}

/**
 * Increments estimated byte length of current active log file.
 * @param increment additional bytes written to log file.
 */
void RollingFileAppender::incrementFileLength(size_t increment)
{
	_priv->fileLength += increment;
}
