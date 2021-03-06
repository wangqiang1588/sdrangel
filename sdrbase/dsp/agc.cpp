/*
 * agc.cpp
 *
 *  Created on: Sep 7, 2015
 *      Author: f4exb
 */

#include <algorithm>
#include "dsp/agc.h"

#include "util/stepfunctions.h"

#define StepLengthMax 2400 // 50ms

AGC::AGC(int historySize, double R) :
	m_u0(1.0),
	m_R(R),
	m_moving_average(historySize, m_R),
	m_historySize(historySize),
	m_count(0)
{}

AGC::~AGC()
{}

void AGC::resize(int historySize, double R)
{
	m_R = R;
	m_moving_average.resize(historySize, R);
	m_historySize = historySize;
	m_count = 0;
}

Real AGC::getValue()
{
	return m_u0;
}

Real AGC::getAverage()
{
	return m_moving_average.average();
}


MagAGC::MagAGC(int historySize, double R, double threshold) :
	AGC(historySize, R),
	m_squared(false),
	m_magsq(0.0),
	m_threshold(threshold),
	m_thresholdEnable(true),
	m_gate(0),
	m_stepLength(std::min(StepLengthMax, historySize/2)),
    m_stepDelta(1.0/m_stepLength),
	m_stepUpCounter(0),
    m_stepDownCounter(m_stepLength),
	m_gateCounter(0),
	m_stepDownDelay(historySize),
	m_clamping(false),
	m_R2(R*R),
	m_clampMax(1.0)
{}

MagAGC::~MagAGC()
{}

void MagAGC::resize(int historySize, Real R)
{
    m_R2 = R*R;
    m_stepLength = std::min(StepLengthMax, historySize/2);
    m_stepDelta = 1.0 / m_stepLength;
    m_stepUpCounter = 0;
    m_stepDownCounter = m_stepLength;
    AGC::resize(historySize, R);
    m_moving_average.fill(0);
}

void MagAGC::setOrder(double R)
{
    m_R2 = R*R;
    AGC::setOrder(R);
    m_moving_average.fill(0);
}

void MagAGC::setThresholdEnable(bool enable)
{
    if (m_thresholdEnable != enable)
    {
        m_stepUpCounter = 0;
        m_stepDownCounter = m_stepLength;
    }

    m_thresholdEnable = enable;
}

void MagAGC::feed(Complex& ci)
{
	ci *= feedAndGetValue(ci);
}

double MagAGC::feedAndGetValue(const Complex& ci)
{
    m_magsq = ci.real()*ci.real() + ci.imag()*ci.imag();
    m_moving_average.feed(m_magsq);

    if (m_clamping)
    {
        if (m_squared)
        {
            double u0 = m_R / m_moving_average.average();
            m_u0 = (u0 * m_magsq > m_clampMax) ? m_clampMax / m_magsq : u0;
        }
        else
        {
            double u02 = m_R2 / m_moving_average.average();
            m_u0 = (u02 * m_magsq > m_clampMax) ? sqrt(m_clampMax / m_magsq) : sqrt(u02);
        }
    }
    else
    {
        m_u0 = m_R / (m_squared ? m_moving_average.average() : sqrt(m_moving_average.average()));
    }

    if (m_thresholdEnable)
    {
        if (m_magsq > m_threshold)
        {
            if (m_gateCounter < m_gate)
            {
                m_gateCounter++;
            }
            else
            {
                m_count = 0;
            }
        }
        else
        {
            if (m_count < m_stepDownDelay) {
                m_count++;
            }

            m_gateCounter = 0;
        }

        if (m_count <  m_stepDownDelay)
        {
            m_stepDownCounter = m_stepUpCounter;

            if (m_stepUpCounter < m_stepLength)
            {
                m_stepUpCounter++;
                return m_u0 * StepFunctions::smootherstep(m_stepUpCounter * m_stepDelta);
            }
            else
            {
                return m_u0;
            }
        }
        else
        {
            m_stepUpCounter = m_stepDownCounter;

            if (m_stepDownCounter > 0)
            {
                m_stepDownCounter--;
                return m_u0 * StepFunctions::smootherstep(m_stepDownCounter * m_stepDelta);
            }
            else
            {
                return 0.0;
            }
        }
    }
    else
    {
        return m_u0;
    }
}
