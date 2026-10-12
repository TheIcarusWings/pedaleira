// The tone request's state, shared by AgentHub.cpp (which owns it) and
// ToneRequestCommands.cpp (which changes it). Private to the hub.
#pragma once

#include "AgentHub.h"

#include <juce_core/juce_core.h>

struct AgentHub::ToneRequest {
  juce::String id;
  // waiting (for an agent to claim it), working, ready (a proposal is in),
  // applied, stopped.
  juce::String status{"waiting"};
  juce::String prompt;
  juce::String guitar;
  juce::String reference;
  juce::String target;
  juce::var limits;
  juce::Time created;

  // The session working on it; 0 while none is.
  int claimedBy = 0;
  juce::String agentName;

  struct Step {
    juce::Time time;
    juce::String text;
  };
  std::vector<Step> steps;
  int step = 0;
  int stepsTotal = 0;

  // The refine conversation, both sides, in order.
  struct Message {
    juce::Time time;
    juce::String from;  // "user" or "agent"
    juce::String text;
  };
  std::vector<Message> messages;
  // How many of the messages the agent has seen (the user's since then are
  // what it has to answer).
  int messagesSeen = 0;

  // The proposal: the agent's sandbox holding the tone, what it says about
  // it and what it changed.
  juce::String sandbox;
  juce::String title;
  juce::String summary;
  juce::var changes;
  juce::var measured;
};
