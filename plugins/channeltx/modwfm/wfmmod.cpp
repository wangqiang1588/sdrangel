///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <QTime>
#include <QDebug>
#include <QMutexLocker>

#include <stdio.h>
#include <complex.h>
#include <algorithm>

#include "dsp/upchannelizer.h"
#include "dsp/dspengine.h"
#include "dsp/threadedbasebandsamplesource.h"
#include "dsp/dspcommands.h"
#include "device/devicesinkapi.h"

#include "wfmmod.h"

MESSAGE_CLASS_DEFINITION(WFMMod::MsgConfigureWFMMod, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgConfigureChannelizer, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgConfigureFileSourceName, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgConfigureFileSourceSeek, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgConfigureAFInput, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgConfigureFileSourceStreamTiming, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgReportFileSourceStreamData, Message)
MESSAGE_CLASS_DEFINITION(WFMMod::MsgReportFileSourceStreamTiming, Message)

const QString WFMMod::m_channelIdURI = "sdrangel.channeltx.modwfm";
const QString WFMMod::m_channelId = "WFMMod";
const int WFMMod::m_levelNbSamples = 480; // every 10ms
const int WFMMod::m_rfFilterFFTLength = 1024;

WFMMod::WFMMod(DeviceSinkAPI *deviceAPI) :
    ChannelSourceAPI(m_channelIdURI),
    m_deviceAPI(deviceAPI),
    m_basebandSampleRate(384000),
    m_outputSampleRate(384000),
    m_inputFrequencyOffset(0),
	m_modPhasor(0.0f),
    m_audioFifo(4800),
	m_settingsMutex(QMutex::Recursive),
    m_fileSize(0),
	m_recordLength(0),
	m_sampleRate(48000),
	m_afInput(WFMModInputNone),
	m_levelCalcCount(0),
	m_peakLevel(0.0f),
	m_levelSum(0.0f)
{
	setObjectName(m_channelId);

    m_rfFilter = new fftfilt(-62500.0 / 384000.0, 62500.0 / 384000.0, m_rfFilterFFTLength);
    m_rfFilterBuffer = new Complex[m_rfFilterFFTLength];
    memset(m_rfFilterBuffer, 0, sizeof(Complex)*(m_rfFilterFFTLength));
    m_rfFilterBufferIndex = 0;

	m_audioBuffer.resize(1<<14);
	m_audioBufferFill = 0;

	m_magsq = 0.0;

	DSPEngine::instance()->getAudioDeviceManager()->addAudioSource(&m_audioFifo, getInputMessageQueue());
    m_audioSampleRate = DSPEngine::instance()->getAudioDeviceManager()->getInputSampleRate();

	m_toneNcoRF.setFreq(1000.0, m_outputSampleRate);

    // CW keyer
    m_cwKeyer.setSampleRate(m_outputSampleRate);
    m_cwKeyer.setWPM(13);
    m_cwKeyer.setMode(CWKeyerSettings::CWNone);
    m_cwKeyer.reset();

    applyChannelSettings(m_basebandSampleRate, m_outputSampleRate, m_inputFrequencyOffset, true);
    applySettings(m_settings, true);

    m_channelizer = new UpChannelizer(this);
    m_threadedChannelizer = new ThreadedBasebandSampleSource(m_channelizer, this);
    m_deviceAPI->addThreadedSource(m_threadedChannelizer);
    m_deviceAPI->addChannelAPI(this);
}

WFMMod::~WFMMod()
{
    delete m_rfFilter;
    delete[] m_rfFilterBuffer;
    DSPEngine::instance()->getAudioDeviceManager()->removeAudioSource(&m_audioFifo);
    m_deviceAPI->removeChannelAPI(this);
    m_deviceAPI->removeThreadedSource(m_threadedChannelizer);
    delete m_threadedChannelizer;
    delete m_channelizer;
}

void WFMMod::pull(Sample& sample)
{
	if (m_settings.m_channelMute)
	{
		sample.m_real = 0.0f;
		sample.m_imag = 0.0f;
		return;
	}

	Complex ci, ri;
    fftfilt::cmplx *rf;
    int rf_out;

	m_settingsMutex.lock();

	if ((m_afInput == WFMModInputFile) || (m_afInput == WFMModInputAudio))
	{
	    if (m_interpolator.interpolate(&m_interpolatorDistanceRemain, m_modSample, &ri))
	    {
	        pullAF(m_modSample);
	        calculateLevel(m_modSample.real());
	        m_audioBufferFill++;
	    }

	    m_interpolatorDistanceRemain += m_interpolatorDistance;
	}
	else
	{
	    pullAF(ri);
	}

    m_modPhasor += (m_settings.m_fmDeviation / (float) m_outputSampleRate) * ri.real() * M_PI * 2.0f;
    ci.real(cos(m_modPhasor) * 0.891235351562f * SDR_TX_SCALEF); // -1 dB
    ci.imag(sin(m_modPhasor) * 0.891235351562f * SDR_TX_SCALEF);

    // RF filtering
    rf_out = m_rfFilter->runFilt(ci, &rf);

    if (rf_out > 0)
    {
        memcpy((void *) m_rfFilterBuffer, (const void *) rf, rf_out*sizeof(Complex));
        m_rfFilterBufferIndex = 0;

    }

    ci = m_rfFilterBuffer[m_rfFilterBufferIndex] * m_carrierNco.nextIQ(); // shift to carrier frequency
    m_rfFilterBufferIndex++;

    m_settingsMutex.unlock();

    double magsq = ci.real() * ci.real() + ci.imag() * ci.imag();
	magsq /= (SDR_TX_SCALED*SDR_TX_SCALED);
	m_movingAverage(magsq);
	m_magsq = m_movingAverage.asDouble();

	sample.m_real = (FixReal) ci.real();
	sample.m_imag = (FixReal) ci.imag();
}

void WFMMod::pullAudio(int nbSamples)
{
    unsigned int nbSamplesAudio = nbSamples * ((Real) m_audioSampleRate / (Real) m_basebandSampleRate);

    if (nbSamplesAudio > m_audioBuffer.size())
    {
        m_audioBuffer.resize(nbSamplesAudio);
    }

    m_audioFifo.read(reinterpret_cast<quint8*>(&m_audioBuffer[0]), nbSamplesAudio, 10);
    m_audioBufferFill = 0;
}

void WFMMod::pullAF(Complex& sample)
{
    switch (m_afInput)
    {
    case WFMModInputTone:
        sample.real(m_toneNcoRF.next() * m_settings.m_volumeFactor);
        sample.imag(0.0f);
        break;
    case WFMModInputFile:
        // sox f4exb_call.wav --encoding float --endian little f4exb_call.raw
        // ffplay -f f32le -ar 48k -ac 1 f4exb_call.raw
        if (m_ifstream.is_open())
        {
            if (m_ifstream.eof())
            {
            	if (m_settings.m_playLoop)
            	{
                    m_ifstream.clear();
                    m_ifstream.seekg(0, std::ios::beg);
            	}
            }

            if (m_ifstream.eof())
            {
            	sample.real(0.0f);
                sample.imag(0.0f);
            }
            else
            {
                Real s;
            	m_ifstream.read(reinterpret_cast<char*>(&s), sizeof(Real));
            	sample.real(s * m_settings.m_volumeFactor);
                sample.imag(0.0f);
            }
        }
        else
        {
            sample.real(0.0f);
            sample.imag(0.0f);
        }
        break;
    case WFMModInputAudio:
        {
            sample.real(((m_audioBuffer[m_audioBufferFill].l + m_audioBuffer[m_audioBufferFill].r) / 65536.0f) * m_settings.m_volumeFactor);
            sample.imag(0.0f);
        }
        break;
    case WFMModInputCWTone:
        Real fadeFactor;

        if (m_cwKeyer.getSample())
        {
            m_cwKeyer.getCWSmoother().getFadeSample(true, fadeFactor);
            sample.real(m_toneNcoRF.next() * m_settings.m_volumeFactor * fadeFactor);
            sample.imag(0.0f);
        }
        else
        {
            if (m_cwKeyer.getCWSmoother().getFadeSample(false, fadeFactor))
            {
                sample.real(m_toneNcoRF.next() * m_settings.m_volumeFactor * fadeFactor);
                sample.imag(0.0f);
            }
            else
            {
                sample.real(0.0f);
                sample.imag(0.0f);
                m_toneNcoRF.setPhase(0);
            }
        }
        break;
    case WFMModInputNone:
    default:
        sample.real(0.0f);
        sample.imag(0.0f);
        break;
    }
}

void WFMMod::calculateLevel(const Real& sample)
{
    if (m_levelCalcCount < m_levelNbSamples)
    {
        m_peakLevel = std::max(std::fabs(m_peakLevel), sample);
        m_levelSum += sample * sample;
        m_levelCalcCount++;
    }
    else
    {
        qreal rmsLevel = sqrt(m_levelSum / m_levelNbSamples);
        //qDebug("WFMMod::calculateLevel: %f %f", rmsLevel, m_peakLevel);
        emit levelChanged(rmsLevel, m_peakLevel, m_levelNbSamples);
        m_peakLevel = 0.0f;
        m_levelSum = 0.0f;
        m_levelCalcCount = 0;
    }
}

void WFMMod::start()
{
	qDebug() << "WFMMod::start: m_outputSampleRate: " << m_outputSampleRate
			<< " m_inputFrequencyOffset: " << m_inputFrequencyOffset;

	m_audioFifo.clear();
	applyChannelSettings(m_basebandSampleRate, m_outputSampleRate, m_inputFrequencyOffset, true);
}

void WFMMod::stop()
{
}

bool WFMMod::handleMessage(const Message& cmd)
{
	if (UpChannelizer::MsgChannelizerNotification::match(cmd))
	{
		UpChannelizer::MsgChannelizerNotification& notif = (UpChannelizer::MsgChannelizerNotification&) cmd;
		qDebug() << "WFMMod::handleMessage: MsgChannelizerNotification";

		applyChannelSettings(notif.getBasebandSampleRate(), notif.getSampleRate(), notif.getFrequencyOffset());

		return true;
	}
    else if (MsgConfigureChannelizer::match(cmd))
    {
        MsgConfigureChannelizer& cfg = (MsgConfigureChannelizer&) cmd;
        qDebug() << "WFMMod::handleMessage: MsgConfigureChannelizer:"
                << " getSampleRate: " << cfg.getSampleRate()
                << " getCenterFrequency: " << cfg.getCenterFrequency();

        m_channelizer->configure(m_channelizer->getInputMessageQueue(),
            cfg.getSampleRate(),
            cfg.getCenterFrequency());

        return true;
    }
    else if (MsgConfigureWFMMod::match(cmd))
    {
        MsgConfigureWFMMod& cfg = (MsgConfigureWFMMod&) cmd;
        qDebug() << "NFWFMMod::handleMessage: MsgConfigureWFMMod";

        WFMModSettings settings = cfg.getSettings();

        applySettings(cfg.getSettings(), cfg.getForce());

        return true;
    }
	else if (MsgConfigureFileSourceName::match(cmd))
    {
        MsgConfigureFileSourceName& conf = (MsgConfigureFileSourceName&) cmd;
        m_fileName = conf.getFileName();
        openFileStream();
        return true;
    }
    else if (MsgConfigureFileSourceSeek::match(cmd))
    {
        MsgConfigureFileSourceSeek& conf = (MsgConfigureFileSourceSeek&) cmd;
        int seekPercentage = conf.getPercentage();
        seekFileStream(seekPercentage);

        return true;
    }
    else if (MsgConfigureAFInput::match(cmd))
    {
        MsgConfigureAFInput& conf = (MsgConfigureAFInput&) cmd;
        m_afInput = conf.getAFInput();

        return true;
    }
    else if (MsgConfigureFileSourceStreamTiming::match(cmd))
    {
    	std::size_t samplesCount;

    	if (m_ifstream.eof()) {
    		samplesCount = m_fileSize / sizeof(Real);
    	} else {
    		samplesCount = m_ifstream.tellg() / sizeof(Real);
    	}

    	MsgReportFileSourceStreamTiming *report;
        report = MsgReportFileSourceStreamTiming::create(samplesCount);
        getMessageQueueToGUI()->push(report);

        return true;
    }
    else if (DSPConfigureAudio::match(cmd))
    {
        DSPConfigureAudio& cfg = (DSPConfigureAudio&) cmd;
        uint32_t sampleRate = cfg.getSampleRate();

        qDebug() << "WFMMod::handleMessage: DSPConfigureAudio:"
                << " sampleRate: " << sampleRate;

        if (sampleRate != m_audioSampleRate) {
            applyAudioSampleRate(sampleRate);
        }

        return true;
    }
    else if (DSPSignalNotification::match(cmd))
    {
        return true;
    }
	else
	{
		return false;
	}
}

void WFMMod::openFileStream()
{
    if (m_ifstream.is_open()) {
        m_ifstream.close();
    }

    m_ifstream.open(m_fileName.toStdString().c_str(), std::ios::binary | std::ios::ate);
    m_fileSize = m_ifstream.tellg();
    m_ifstream.seekg(0,std::ios_base::beg);

    m_sampleRate = 48000; // fixed rate
    m_recordLength = m_fileSize / (sizeof(Real) * m_sampleRate);

    qDebug() << "WFMMod::openFileStream: " << m_fileName.toStdString().c_str()
            << " fileSize: " << m_fileSize << "bytes"
            << " length: " << m_recordLength << " seconds";

    MsgReportFileSourceStreamData *report;
    report = MsgReportFileSourceStreamData::create(m_sampleRate, m_recordLength);
    getMessageQueueToGUI()->push(report);
}

void WFMMod::seekFileStream(int seekPercentage)
{
    QMutexLocker mutexLocker(&m_settingsMutex);

    if (m_ifstream.is_open())
    {
        int seekPoint = ((m_recordLength * seekPercentage) / 100) * m_sampleRate;
        seekPoint *= sizeof(Real);
        m_ifstream.clear();
        m_ifstream.seekg(seekPoint, std::ios::beg);
    }
}

void WFMMod::applyAudioSampleRate(int sampleRate)
{
    qDebug("WFMMod::applyAudioSampleRate: %d", sampleRate);

    m_settingsMutex.lock();
    m_interpolatorDistanceRemain = 0;
    m_interpolatorConsumed = false;
    m_interpolatorDistance = (Real) sampleRate / (Real) m_outputSampleRate;
    m_interpolator.create(48, sampleRate, m_settings.m_rfBandwidth / 2.2, 3.0);
    m_settingsMutex.unlock();

    m_audioSampleRate = sampleRate;
}

void WFMMod::applyChannelSettings(int basebandSampleRate, int outputSampleRate, int inputFrequencyOffset, bool force)
{
    qDebug() << "WFMMod::applyChannelSettings:"
            << " basebandSampleRate: " << basebandSampleRate
            << " outputSampleRate: " << outputSampleRate
            << " inputFrequencyOffset: " << inputFrequencyOffset;

    if ((inputFrequencyOffset != m_inputFrequencyOffset) ||
        (outputSampleRate != m_outputSampleRate) || force)
    {
        m_settingsMutex.lock();
        m_carrierNco.setFreq(inputFrequencyOffset, outputSampleRate);
        m_settingsMutex.unlock();
    }

    if ((outputSampleRate != m_outputSampleRate) || force)
    {
        m_settingsMutex.lock();
        m_interpolatorDistanceRemain = 0;
        m_interpolatorConsumed = false;
        m_interpolatorDistance = (Real) m_audioSampleRate / (Real) outputSampleRate;
        m_interpolator.create(48, m_audioSampleRate, m_settings.m_rfBandwidth / 2.2, 3.0);
        Real lowCut = -(m_settings.m_rfBandwidth / 2.0) / outputSampleRate;
        Real hiCut  = (m_settings.m_rfBandwidth / 2.0) / outputSampleRate;
        m_rfFilter->create_filter(lowCut, hiCut);
        m_toneNcoRF.setFreq(m_settings.m_toneFrequency, outputSampleRate);
        m_cwKeyer.setSampleRate(outputSampleRate);
        m_cwKeyer.reset();
        m_settingsMutex.unlock();
    }

    m_basebandSampleRate = basebandSampleRate;
    m_outputSampleRate = outputSampleRate;
    m_inputFrequencyOffset = inputFrequencyOffset;
}

void WFMMod::applySettings(const WFMModSettings& settings, bool force)
{
    qDebug() << "WFMMod::applySettings:"
            << " m_inputFrequencyOffset: " << settings.m_inputFrequencyOffset
            << " m_rfBandwidth: " << settings.m_rfBandwidth
            << " m_afBandwidth: " << settings.m_afBandwidth
            << " m_fmDeviation: " << settings.m_fmDeviation
            << " m_volumeFactor: " << settings.m_volumeFactor
            << " m_toneFrequency: " << settings.m_toneFrequency
            << " m_channelMute: " << settings.m_channelMute
            << " m_playLoop: " << settings.m_playLoop
            << " m_audioDeviceName: " << settings.m_audioDeviceName
            << " force: " << force;

    if((settings.m_afBandwidth != m_settings.m_afBandwidth) || force)
    {
        m_settingsMutex.lock();
        m_interpolatorDistanceRemain = 0;
        m_interpolatorConsumed = false;
        m_interpolatorDistance = (Real) m_audioSampleRate / (Real) m_outputSampleRate;
        m_interpolator.create(48, m_audioSampleRate, settings.m_rfBandwidth / 2.2, 3.0);
        m_settingsMutex.unlock();
    }

    if ((settings.m_rfBandwidth != m_settings.m_rfBandwidth) || force)
    {
        m_settingsMutex.lock();
        Real lowCut = -(settings.m_rfBandwidth / 2.0) / m_outputSampleRate;
        Real hiCut  = (settings.m_rfBandwidth / 2.0) / m_outputSampleRate;
        m_rfFilter->create_filter(lowCut, hiCut);
        m_settingsMutex.unlock();
    }

    if ((settings.m_toneFrequency != m_settings.m_toneFrequency) || force)
    {
        m_settingsMutex.lock();
        m_toneNcoRF.setFreq(settings.m_toneFrequency, m_outputSampleRate);
        m_settingsMutex.unlock();
    }

    if ((settings.m_audioDeviceName != m_settings.m_audioDeviceName) || force)
    {
        AudioDeviceManager *audioDeviceManager = DSPEngine::instance()->getAudioDeviceManager();
        int audioDeviceIndex = audioDeviceManager->getInputDeviceIndex(settings.m_audioDeviceName);
        audioDeviceManager->addAudioSource(&m_audioFifo, getInputMessageQueue(), audioDeviceIndex);
        uint32_t audioSampleRate = audioDeviceManager->getInputSampleRate(audioDeviceIndex);

        if (m_audioSampleRate != audioSampleRate) {
            applyAudioSampleRate(audioSampleRate);
        }
    }

    m_settings = settings;
}

QByteArray WFMMod::serialize() const
{
    return m_settings.serialize();
}

bool WFMMod::deserialize(const QByteArray& data)
{
    if (m_settings.deserialize(data))
    {
        MsgConfigureWFMMod *msg = MsgConfigureWFMMod::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return true;
    }
    else
    {
        m_settings.resetToDefaults();
        MsgConfigureWFMMod *msg = MsgConfigureWFMMod::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return false;
    }
}
