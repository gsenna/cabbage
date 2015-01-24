/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2013 - Raw Material Software Ltd.

   Permission is granted to use this software under the terms of either:
   a) the GPL v2 (or any later version)
   b) the Affero GPL v3

   Details of these licenses can be found at: www.gnu.org/licenses

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

   ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.juce.com for more information.

  ==============================================================================
*/

#include "../JuceLibraryCode/JuceHeader.h"
#include "GraphEditorPanel.h"
#include "InternalFilters.h"
#include "MainHostWindow.h"
#include "PluginWrapperProcessor.h"


//==============================================================================
class PluginWindow;
static Array <PluginWindow*> activePluginWindows;

PluginWindow::PluginWindow (Component* const pluginEditor,
                            AudioProcessorGraph::Node* const o,
                            WindowFormatType t)
    : DocumentWindow (pluginEditor->getName(), Colours::lightblue,
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton),
    owner (o),
    type (t)
{
    setSize (400, 300);

    setContentOwned (pluginEditor, true);

    setTopLeftPosition (owner->properties.getWithDefault ("uiLastX", Random::getSystemRandom().nextInt (500)),
                        owner->properties.getWithDefault ("uiLastY", Random::getSystemRandom().nextInt (500)));
    setVisible (true);

    activePluginWindows.add (this);
}

void PluginWindow::closeCurrentlyOpenWindowsFor (const uint32 nodeId)
{
    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner->nodeId == nodeId)
            delete activePluginWindows.getUnchecked (i);
}

void PluginWindow::closeAllCurrentlyOpenWindows()
{
    if (activePluginWindows.size() > 0)
    {
        for (int i = activePluginWindows.size(); --i >= 0;)
            delete activePluginWindows.getUnchecked (i);

        Component dummyModalComp;
        dummyModalComp.enterModalState();
        MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
}

//==============================================================================
class ProcessorProgramPropertyComp : public PropertyComponent,
    private AudioProcessorListener
{
public:
    ProcessorProgramPropertyComp (const String& name, AudioProcessor& p, int index_)
        : PropertyComponent (name),
          owner (p),
          index (index_)
    {
        owner.addListener (this);
    }

    ~ProcessorProgramPropertyComp()
    {
        owner.removeListener (this);
    }

    void refresh() { }
    void audioProcessorChanged (AudioProcessor*) { }
    void audioProcessorParameterChanged (AudioProcessor*, int, float) { }

private:
    AudioProcessor& owner;
    const int index;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessorProgramPropertyComp)
};

class ProgramAudioProcessorEditor : public AudioProcessorEditor
{
public:
    ProgramAudioProcessorEditor (AudioProcessor* const p)
        : AudioProcessorEditor (p)
    {
        jassert (p != nullptr);
        setOpaque (true);

        addAndMakeVisible (panel);

        Array<PropertyComponent*> programs;

        const int numPrograms = p->getNumPrograms();
        int totalHeight = 0;

        for (int i = 0; i < numPrograms; ++i)
        {
            String name (p->getProgramName (i).trim());

            if (name.isEmpty())
                name = "Unnamed";

            ProcessorProgramPropertyComp* const pc = new ProcessorProgramPropertyComp (name, *p, i);
            programs.add (pc);
            totalHeight += pc->getPreferredHeight();
        }

        panel.addProperties (programs);

        setSize (400, jlimit (25, 400, totalHeight));
    }

    void paint (Graphics& g)
    {
        g.fillAll (Colours::grey);
    }

    void resized()
    {
        panel.setBounds (getLocalBounds());
    }

private:
    PropertyPanel panel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramAudioProcessorEditor)
};

//==============================================================================
PluginWindow* PluginWindow::getWindowFor (AudioProcessorGraph::Node* const node,
        WindowFormatType type)
{
    jassert (node != nullptr);

    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner == node
                && activePluginWindows.getUnchecked(i)->type == type)
            return activePluginWindows.getUnchecked(i);

    AudioProcessor* processor = node->getProcessor();
    AudioProcessorEditor* ui = nullptr;

    if (type == Normal)
    {
        ui = processor->createEditorIfNeeded();

        Logger::writeToLog("Width:"+String(ui->getWidth()));
        Logger::writeToLog("Height:"+String(ui->getHeight()));

        if (ui == nullptr)
            type = Generic;
    }

    if (ui == nullptr)
    {
        if (type == Generic || type == Parameters)
            ui = new GenericAudioProcessorEditor (processor);
        else if (type == Programs)
            ui = new ProgramAudioProcessorEditor (processor);
    }

    if (ui != nullptr)
    {
        if (AudioPluginInstance* const plugin = dynamic_cast<AudioPluginInstance*> (processor))
            ui->setName (plugin->getName());

        return new PluginWindow (ui, node, type);
    }

    return nullptr;
}

PluginWindow::~PluginWindow()
{
    activePluginWindows.removeFirstMatchingValue (this);
    clearContentComponent();
}

void PluginWindow::moved()
{
    owner->properties.set ("uiLastX", getX());
    owner->properties.set ("uiLastY", getY());
}

void PluginWindow::closeButtonPressed()
{
    delete this;
}

//==============================================================================
class PinComponent   : public Component,
    public SettableTooltipClient
{
public:
    PinComponent (FilterGraph& graph_,
                  const uint32 filterID_, const int index_, const bool isInput_)
        : filterID (filterID_),
          index (index_),
          isInput (isInput_),
          graph (graph_)
    {
        if (const AudioProcessorGraph::Node::Ptr node = graph.getNodeForId (filterID_))
        {
            String tip;

            if (index_ == FilterGraph::midiChannelNumber)
            {
                tip = isInput ? "MIDI Input" : "MIDI Output";
            }
            else
            {
                if (isInput)
                    tip = node->getProcessor()->getInputChannelName (index_);
                else
                    tip = node->getProcessor()->getOutputChannelName (index_);

                if (tip.isEmpty())
                    tip = (isInput ? "Input " : "Output ") + String (index_ + 1);
            }

            setTooltip (tip);
        }

        setSize (16, 16);
    }

    void paint (Graphics& g)
    {
        const float w = (float) getWidth();
        const float h = (float) getHeight();

        Path p;
        p.addEllipse (w * 0.25f, h * 0.25f, w * 0.5f, h * 0.5f);

        p.addRectangle (w * 0.4f, isInput ? (0.5f * h) : 0.0f, w * 0.2f, h * 0.5f);

        g.setColour (index == FilterGraph::midiChannelNumber ? Colours::cornflowerblue : Colours::green);
        g.fillPath (p);
    }

    void mouseDown (const MouseEvent& e)
    {
        getGraphPanel()->beginConnectorDrag (isInput ? 0 : filterID,
                                             index,
                                             isInput ? filterID : 0,
                                             index,
                                             e);
    }

    void mouseDrag (const MouseEvent& e)
    {
        getGraphPanel()->dragConnector (e);
    }

    void mouseUp (const MouseEvent& e)
    {
        getGraphPanel()->endDraggingConnector (e);
    }

    const uint32 filterID;
    const int index;
    const bool isInput;

private:
    FilterGraph& graph;

    GraphEditorPanel* getGraphPanel() const noexcept
    {
        return findParentComponentOfClass<GraphEditorPanel>();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PinComponent)
};

//================================================================================
FilterComponent::FilterComponent (FilterGraph& graph_, const uint32 filterID_)
        : graph (graph_),
          filterID (filterID_),
          numInputs (0),
          numOutputs (0),
          pinSize (16),
          font (13.0f, Font::bold),
          numIns (0),
          numOuts (0),
		  rmsLeft(0),
		  rmsRight(0),
		  filterIsPartofSelectedGroup(false)
{
	shadow.setShadowProperties (DropShadow (Colours::black.withAlpha (0.5f), 3, Point<int> (0, 1)));
	setComponentEffect (&shadow);
	//setSize (150, 90);		
}
//================================================================================
FilterComponent::~FilterComponent()
{
	deleteAllChildren();
}
//================================================================================	
void FilterComponent::mouseDown (const MouseEvent& e)
{

	Logger::writeToLog("NodeID: "+String(filterID));
	getGraphPanel()->selectedFilterCoordinates.clear();

	int numSelected = getGraphPanel()->getLassoSelection().getNumSelected();
	for(int i=0; i<numSelected; i++)
	{
		//check if current filter is part of a group or not, if so add position to coor array
		if(filterID==getGraphPanel()->getLassoSelection().getSelectedItem(i)->filterID)
			filterIsPartofSelectedGroup = true;
		getGraphPanel()->selectedFilterCoordinates.add(getGraphPanel()->getLassoSelection().getSelectedItem(i)->getPosition());
	}

	if(!filterIsPartofSelectedGroup) {
		for(int i=0; i<getGraphPanel()->getNumChildComponents(); i++) {
			//if not part of a group reset colours back to normal
			getGraphPanel()->getChildComponent(i)->getProperties().set("colour", "");
			getGraphPanel()->getChildComponent(i)->repaint();
		}
		getGraphPanel()->getLassoSelection().deselectAll();
	}

	originalPos = localPointToGlobal (Point<int>());

	toFront (true);

	if (e.mods.isPopupMenu())
	{
		PopupMenu m;
		m.addItem (1, "Delete this filter");
		m.addItem (2, "Disconnect all pins");
		m.addSeparator();
		m.addItem (3, "Show plugin UI");
		m.addItem (4, "Show all programs");
		m.addItem (5, "Show all parameters");
		m.addItem (6, "Test state save/load");

		const int r = m.show();

		if (r == 1)
		{
			graph.removeFilter (filterID);
			return;
		}
		else if (r == 2)
		{
			graph.disconnectFilter (filterID);
		}
		else
		{
			if (AudioProcessorGraph::Node::Ptr f = graph.getNodeForId (filterID))
			{
				AudioProcessor* const processor = f->getProcessor();
				jassert (processor != nullptr);

				if (r == 6)
				{
					MemoryBlock state;
					processor->getStateInformation (state);
					processor->setStateInformation (state.getData(), (int) state.getSize());
				}
				else
				{
					PluginWindow::WindowFormatType type = processor->hasEditor() ? PluginWindow::Normal
														  : PluginWindow::Generic;

					switch (r)
					{
					case 4:
						type = PluginWindow::Programs;
						break;
					case 5:
						type = PluginWindow::Parameters;
						break;

					default:
						break;
					};

					if (PluginWindow* const w = PluginWindow::getWindowFor (f, type))
						w->toFront (true);
				}
			}
		}
	}
}
//================================================================================
void FilterComponent::mouseDrag (const MouseEvent& e)
{
	if (! e.mods.isPopupMenu())
	{
		Point<int> pos (originalPos + Point<int> (e.getDistanceFromDragStartX(), e.getDistanceFromDragStartY()));

		if (getParentComponent() != nullptr)
			pos = getParentComponent()->getLocalPoint (nullptr, pos);

		int numSelected = getGraphPanel()->getLassoSelection().getNumSelected();

		for(int i=0; i<numSelected; i++)
			if(filterID==getGraphPanel()->getLassoSelection().getSelectedItem(i)->filterID)
				filterIsPartofSelectedGroup = true;

		if(filterIsPartofSelectedGroup)
		{
			for(int i=0; i<numSelected; i++)
			{
				int fltID = getGraphPanel()->getLassoSelection().getSelectedItem(i)->filterID;
				int filterPosX = getGraphPanel()->selectedFilterCoordinates[i].getX();
				int filterPosY = getGraphPanel()->selectedFilterCoordinates[i].getY();

				//Logger::writeToLog("FilterID from Filter Component MouseDrag:"+String(fltID));
				graph.setNodePosition (fltID,
									   (filterPosX+e.getDistanceFromDragStartX() + getWidth() / 2) / (double) getParentWidth(),
									   (filterPosY+e.getDistanceFromDragStartY() + getHeight() / 2) / (double) getParentHeight());
			}
		}
		
		else 
		{
			graph.setNodePosition (filterID,
								   (pos.getX() + getWidth() / 2) / (double) getParentWidth(),
								   (pos.getY() + getHeight() / 2) / (double) getParentHeight());
		}

		getGraphPanel()->updateComponents();
	}
	repaint();
}
//================================================================================
void FilterComponent::mouseUp (const MouseEvent& e)
{
	if (e.mouseWasClicked() && e.getNumberOfClicks() == 2)
	{
		if (const AudioProcessorGraph::Node::Ptr f = graph.getNodeForId (filterID))
			if (PluginWindow* const w = PluginWindow::getWindowFor (f, PluginWindow::Normal))
				w->toFront (true);
	}
	else if (! e.mouseWasClicked())
	{
		graph.setChangedFlag (true);
	}
}
//================================================================================
bool FilterComponent::hitTest (int x, int y)
{
	for (int i = getNumChildComponents(); --i >= 0;)
		if (getChildComponent(i)->getBounds().contains (x, y))
			return true;

	return x >= 3 && x < getWidth() - 6 && y >= pinSize && y < getHeight() - pinSize;
}
//================================================================================
void FilterComponent::actionListenerCallback (const String &message)
{
	rmsLeft = message.substring(0, message.indexOf(" ")).getFloatValue();
	rmsRight = message.substring(message.indexOf(" ")+1).getFloatValue();
	repaint();
}
//================================================================================
void FilterComponent::paint (Graphics& g)
{
	g.fillAll(filterColour);
	String outlineColour = getProperties().getWithDefault("colour", "").toString();
	if(outlineColour.length()>1)
	{
		g.setColour(Colour::fromString(outlineColour));
		
	}
	else
	{
		g.setColour(filterColour);
		filterIsPartofSelectedGroup=false;
	}

	const int x = 4;
	const int y = pinSize;
	const int w = getWidth() - x * 2;
	const int h = getHeight() - pinSize * 2;

	g.drawRoundedRectangle(x, y, w, h, 5, 1.f);
	g.setColour (cUtils::getComponentFontColour());
	g.setFont (cUtils::getComponentFont());
	g.drawFittedText (getName(),
					  x + 4, y - 2, w - 8, h - 4,
					  Justification::centred, 2);

	g.setOpacity(0.2);
	g.drawRoundedRectangle(x+0.5, y+0.5, w-1, h-1, 5, 1.0f);
	
	//g.setColour(Colours::cornflowerblue);	
	
	ColourGradient vuGradient(Colours::lime, 0.f, 0.f, Colours::cornflowerblue, getWidth(), getHeight(), false);
	g.setGradientFill(vuGradient);
	g.fillRoundedRectangle(x+4, h+4.f, (getWidth()-15)*rmsLeft, 4.f, 1.f);
	g.fillRoundedRectangle(x+4, h+9.f, (getWidth()-15)*rmsRight, 4.f, 1.f);
	
}
//================================================================================
void FilterComponent::resized()
{
	for (int i = 0; i < getNumChildComponents(); ++i)
	{
		if (PinComponent* const pc = dynamic_cast <PinComponent*> (getChildComponent(i)))
		{
			const int total = pc->isInput ? numIns : numOuts;
			const int index = pc->index == FilterGraph::midiChannelNumber ? (total - 1) : pc->index;

			pc->setBounds (proportionOfWidth ((1 + index) / (total + 1.0f)) - pinSize / 2,
						   pc->isInput ? 0 : (getHeight() - pinSize),
						   pinSize, pinSize);
		}
	}
}
//================================================================================
void FilterComponent::getPinPos (const int index, const bool isInput, float& x, float& y)
{
	for (int i = 0; i < getNumChildComponents(); ++i)
	{
		if (PinComponent* const pc = dynamic_cast <PinComponent*> (getChildComponent(i)))
		{
			if (pc->index == index && isInput == pc->isInput)
			{
				x = getX() + pc->getX() + pc->getWidth() * 0.5f;
				y = getY() + pc->getY() + pc->getHeight() * 0.5f;
				break;
			}
		}
	}
}

//================================================================================
void FilterComponent::update()
{
	const AudioProcessorGraph::Node::Ptr f (graph.getNodeForId (filterID));

	if (f == nullptr)
	{
		delete this;
		return;
	}

	numIns = f->getProcessor()->getNumInputChannels();
	if (f->getProcessor()->acceptsMidi())
		++numIns;

	numOuts = f->getProcessor()->getNumOutputChannels();
	if (f->getProcessor()->producesMidi())
		++numOuts;

	int w = 100;
	int h = 60;

	w = jmax (w, (jmax (numIns, numOuts) + 1) * 20);

	const int textWidth = font.getStringWidth (f->getProcessor()->getName());
	w = jmax (w, 16 + jmin (textWidth, 300));
	if (textWidth > 300)
		h = 100;

	setSize (w, h);

	PluginWrapperProcessor* tmpPlug = dynamic_cast <PluginWrapperProcessor*> (f->getProcessor());
	
	if(tmpPlug)
	{
		setName (tmpPlug->getPluginName());
		tmpPlug->addActionListener(this);
	}
	else
		setName(f->getProcessor()->getName());

	{
		double x, y;
		graph.getNodePosition (filterID, x, y);
		setCentreRelative ((float) x, (float) y);
	}

	if (numIns != numInputs || numOuts != numOutputs)
	{
		numInputs = numIns;
		numOutputs = numOuts;

		deleteAllChildren();

		int i;
		for (i = 0; i < f->getProcessor()->getNumInputChannels(); ++i)
			addAndMakeVisible (new PinComponent (graph, filterID, i, true));

		if (f->getProcessor()->acceptsMidi())
			addAndMakeVisible (new PinComponent (graph, filterID, FilterGraph::midiChannelNumber, true));

		for (i = 0; i < f->getProcessor()->getNumOutputChannels(); ++i)
			addAndMakeVisible (new PinComponent (graph, filterID, i, false));

		if (f->getProcessor()->producesMidi())
			addAndMakeVisible (new PinComponent (graph, filterID, FilterGraph::midiChannelNumber, false));

		resized();
	}
}

//==============================================================================
class ConnectorComponent   : public Component,
    public SettableTooltipClient
{
public:
    ConnectorComponent (FilterGraph& graph_)
        : sourceFilterID (0),
          destFilterID (0),
          sourceFilterChannel (0),
          destFilterChannel (0),
          graph (graph_),
          lastInputX (0),
          lastInputY (0),
          lastOutputX (0),
          lastOutputY (0)
    {
        setAlwaysOnTop (true);
    }

    void setInput (const uint32 sourceFilterID_, const int sourceFilterChannel_)
    {
        if (sourceFilterID != sourceFilterID_ || sourceFilterChannel != sourceFilterChannel_)
        {
            sourceFilterID = sourceFilterID_;
            sourceFilterChannel = sourceFilterChannel_;
            update();
        }
    }

    void setOutput (const uint32 destFilterID_, const int destFilterChannel_)
    {
        if (destFilterID != destFilterID_ || destFilterChannel != destFilterChannel_)
        {
            destFilterID = destFilterID_;
            destFilterChannel = destFilterChannel_;
            update();
        }
    }

    void dragStart (int x, int y)
    {
        lastInputX = (float) x;
        lastInputY = (float) y;
        resizeToFit();
    }

    void dragEnd (int x, int y)
    {
        lastOutputX = (float) x;
        lastOutputY = (float) y;
        resizeToFit();
    }

    void update()
    {
        float x1, y1, x2, y2;
        getPoints (x1, y1, x2, y2);

        if (lastInputX != x1
                || lastInputY != y1
                || lastOutputX != x2
                || lastOutputY != y2)
        {
            resizeToFit();
        }
    }

    void resizeToFit()
    {
        float x1, y1, x2, y2;
        getPoints (x1, y1, x2, y2);

        const Rectangle<int> newBounds ((int) jmin (x1, x2) - 4,
                                        (int) jmin (y1, y2) - 4,
                                        (int) std::abs (x1 - x2) + 8,
                                        (int) std::abs (y1 - y2) + 8);

        if (newBounds != getBounds())
            setBounds (newBounds);
        else
            resized();

        repaint();
    }

    void getPoints (float& x1, float& y1, float& x2, float& y2) const
    {
        x1 = lastInputX;
        y1 = lastInputY;
        x2 = lastOutputX;
        y2 = lastOutputY;

        if (GraphEditorPanel* const hostPanel = getGraphPanel())
        {
            if (FilterComponent* srcFilterComp = hostPanel->getComponentForFilter (sourceFilterID))
                srcFilterComp->getPinPos (sourceFilterChannel, false, x1, y1);

            if (FilterComponent* dstFilterComp = hostPanel->getComponentForFilter (destFilterID))
                dstFilterComp->getPinPos (destFilterChannel, true, x2, y2);
        }
    }

    void paint (Graphics& g)
    {
        if (sourceFilterChannel == FilterGraph::midiChannelNumber
                || destFilterChannel == FilterGraph::midiChannelNumber)
        {
            g.setColour (Colours::cornflowerblue);
        }
        else
        {
            g.setColour (Colours::green);
        }

        g.fillPath (linePath);
    }

    bool hitTest (int x, int y)
    {
        if (hitPath.contains ((float) x, (float) y))
        {
            double distanceFromStart, distanceFromEnd;
            getDistancesFromEnds (x, y, distanceFromStart, distanceFromEnd);

            // avoid clicking the connector when over a pin
            return distanceFromStart > 7.0 && distanceFromEnd > 7.0;
        }

        return false;
    }

    void mouseDown (const MouseEvent&)
    {
        dragging = false;
    }

    void mouseDrag (const MouseEvent& e)
    {
        if ((! dragging) && ! e.mouseWasClicked())
        {
            dragging = true;

            graph.removeConnection (sourceFilterID, sourceFilterChannel, destFilterID, destFilterChannel);

            double distanceFromStart, distanceFromEnd;
            getDistancesFromEnds (e.x, e.y, distanceFromStart, distanceFromEnd);
            const bool isNearerSource = (distanceFromStart < distanceFromEnd);

            getGraphPanel()->beginConnectorDrag (isNearerSource ? 0 : sourceFilterID,
                                                 sourceFilterChannel,
                                                 isNearerSource ? destFilterID : 0,
                                                 destFilterChannel,
                                                 e);
        }
        else if (dragging)
        {
            getGraphPanel()->dragConnector (e);
        }
    }

    void mouseUp (const MouseEvent& e)
    {
        if (dragging)
            getGraphPanel()->endDraggingConnector (e);
    }

    void resized()
    {
        float x1, y1, x2, y2;
        getPoints (x1, y1, x2, y2);

        lastInputX = x1;
        lastInputY = y1;
        lastOutputX = x2;
        lastOutputY = y2;

        x1 -= getX();
        y1 -= getY();
        x2 -= getX();
        y2 -= getY();

        linePath.clear();
        linePath.startNewSubPath (x1, y1);
        linePath.cubicTo (x1, y1 + (y2 - y1) * 0.33f,
                          x2, y1 + (y2 - y1) * 0.66f,
                          x2, y2);

        PathStrokeType wideStroke (8.0f);
        wideStroke.createStrokedPath (hitPath, linePath);

        PathStrokeType stroke (2.5f);
        stroke.createStrokedPath (linePath, linePath);

        const float arrowW = 5.0f;
        const float arrowL = 4.0f;

        Path arrow;
        arrow.addTriangle (-arrowL, arrowW,
                           -arrowL, -arrowW,
                           arrowL, 0.0f);

        arrow.applyTransform (AffineTransform::identity
                              .rotated (float_Pi * 0.5f - (float) atan2 (x2 - x1, y2 - y1))
                              .translated ((x1 + x2) * 0.5f,
                                           (y1 + y2) * 0.5f));

        linePath.addPath (arrow);
        linePath.setUsingNonZeroWinding (true);
    }

    uint32 sourceFilterID, destFilterID;
    int sourceFilterChannel, destFilterChannel;

private:
    FilterGraph& graph;
    float lastInputX, lastInputY, lastOutputX, lastOutputY;
    Path linePath, hitPath;
    bool dragging;

    GraphEditorPanel* getGraphPanel() const noexcept
    {
        return findParentComponentOfClass<GraphEditorPanel>();
    }

    void getDistancesFromEnds (int x, int y, double& distanceFromStart, double& distanceFromEnd) const
    {
        float x1, y1, x2, y2;
        getPoints (x1, y1, x2, y2);

        distanceFromStart = juce_hypot (x - (x1 - getX()), y - (y1 - getY()));
        distanceFromEnd = juce_hypot (x - (x2 - getX()), y - (y2 - getY()));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConnectorComponent)
};


//==============================================================================
GraphEditorPanel::GraphEditorPanel (FilterGraph& graph_)
    : graph (graph_)
{
    InternalPluginFormat internalFormat;

    graph.addFilter (internalFormat.getDescriptionFor (InternalPluginFormat::audioInputFilter),
               0.5f, 0.2f);

    graph.addFilter (internalFormat.getDescriptionFor (InternalPluginFormat::midiInputFilter),
               0.3f, 0.2f);

    graph.addFilter (internalFormat.getDescriptionFor (InternalPluginFormat::audioOutputFilter),
               0.5f, 0.8f);
    graph.addChangeListener (this);
    setOpaque (true);
}

GraphEditorPanel::~GraphEditorPanel()
{
    graph.removeChangeListener (this);
    draggingConnector = nullptr;
    deleteAllChildren();
}

void GraphEditorPanel::paint (Graphics& g)
{
    g.fillAll (Colour(20, 20, 20));
}

void GraphEditorPanel::mouseDown (const MouseEvent& e)
{
	Array<File> cabbageFiles;
	int numNonNativePlugins;
	
    if (e.mods.isPopupMenu())
    {
        PopupMenu m;

        if (MainHostWindow* const mainWindow = findParentComponentOfClass<MainHostWindow>())
        {
			mainWindow->addPluginsToMenu (m);
			numNonNativePlugins = m.getNumItems();
			mainWindow->addCabbageNativePluginsToMenu(m, cabbageFiles);
			m.addSeparator();
            const int r = m.show();
//			createNewPlugin (mainWindow->getChosenType (r), e.x, e.y, false, "");
//			return;
			Logger::writeToLog("PopupMenu ID: "+String(r));
			if(r>0) //make sure we have a valid item index....
			{
				if(r<numNonNativePlugins)
				{
					createNewPlugin (mainWindow->getChosenType (r), e.x, e.y, false, "");
					return;
				}
				else //if(r>numNonNativePlugins && r<cabbageFiles.size()-4){
				{ 
					Logger::writeToLog(cabbageFiles[r-numNonNativePlugins].getFullPathName());
					createNewPlugin (mainWindow->getChosenType (r), e.x, e.y, true, cabbageFiles[r-numNonNativePlugins].getFullPathName());
					return;
				}
			}

        }
    }
    else
    {
        //if((e.mods.getCurrentModifiers().isCtrlDown())&& (e.mods.isLeftButtonDown())){
        addChildComponent (&lassoComp);
        lassoComp.beginLasso (e, this);
        //}
    }


    //deselect all grouped filters and set alpha to normal
    selectedFilters.deselectAll();
    for(int i=0; i<getNumChildComponents(); i++)
    {
        getChildComponent(i)->getProperties().set("colour", "");
        getChildComponent(i)->repaint();
    }
}


void GraphEditorPanel::mouseDrag (const MouseEvent& e)
{
//	if(!e.mods.isCommandDown())
//		myDragger.dragComponent (this, e, nullptr);
//	else{
    lassoComp.toFront (false);
    lassoComp.dragLasso (e);
//	}
}

void GraphEditorPanel::mouseUp (const MouseEvent& e)
{
    //if a selection has been made update the selected groups alpha setting to highlight selection
    Logger::writeToLog("Number selected: "+String(selectedFilters.getNumSelected()));
    for(int i=0; i<selectedFilters.getNumSelected(); i++) {
        //Logger::writeToLog(getChildComponent(i)->getName());
        selectedFilters.getSelectedItem(i)->getProperties().set("colour", Colours::yellow.toString());
        selectedFilters.getSelectedItem(i)->repaint();
    }

    lassoComp.endLasso();
    removeChildComponent (&lassoComp);
}

void GraphEditorPanel::findLassoItemsInArea (Array <FilterComponent*>& results, const Rectangle<int>& area)
{
    const Rectangle<int> lasso (area);

    for (int i = 0; i < getNumChildComponents()-1; i++)
    {
        FilterComponent* c = (FilterComponent*)getChildComponent(i);
        if (c->getBounds().intersects (lasso)) {
            results.addIfNotAlreadyThere(c);
            selectedFilters.addToSelection(c);
            Logger::writeToLog(c->getName());
        }
        else
            selectedFilters.deselect(c);
    }
}

void GraphEditorPanel::createNewPlugin (const PluginDescription* desc, int x, int y, bool isNative, String fileName)
{
	if(isNative)
	{
		PluginDescription descript;
		descript.fileOrIdentifier = fileName;
		descript.descriptiveName = "Cabbage Plugin "+File(fileName).getFileNameWithoutExtension();
		descript.name = File(fileName).getFileNameWithoutExtension();
		descript.manufacturerName = "Cabbage Foundation";
		descript.numInputChannels = 2;
		descript.pluginFormatName = "Cabbage";
		descript.numOutputChannels = 2;
		graph.addFilter (&descript, x / (double) getWidth(), y / (double) getHeight());
	}
	else
	{
		graph.addFilter (desc, x / (double) getWidth(), y / (double) getHeight());
	}
}

FilterComponent* GraphEditorPanel::getComponentForFilter (const uint32 filterID) const
{
    for (int i = getNumChildComponents(); --i >= 0;)
    {
        if (FilterComponent* const fc = dynamic_cast <FilterComponent*> (getChildComponent (i)))
            if (fc->filterID == filterID)
                return fc;
    }

    return nullptr;
}

ConnectorComponent* GraphEditorPanel::getComponentForConnection (const AudioProcessorGraph::Connection& conn) const
{
    for (int i = getNumChildComponents(); --i >= 0;)
    {
        if (ConnectorComponent* const c = dynamic_cast <ConnectorComponent*> (getChildComponent (i)))
            if (c->sourceFilterID == conn.sourceNodeId
                    && c->destFilterID == conn.destNodeId
                    && c->sourceFilterChannel == conn.sourceChannelIndex
                    && c->destFilterChannel == conn.destChannelIndex)
                return c;
    }

    return nullptr;
}

PinComponent* GraphEditorPanel::findPinAt (const int x, const int y) const
{
    for (int i = getNumChildComponents(); --i >= 0;)
    {
        if (FilterComponent* fc = dynamic_cast <FilterComponent*> (getChildComponent (i)))
        {
            if (PinComponent* pin = dynamic_cast <PinComponent*> (fc->getComponentAt (x - fc->getX(),
                                    y - fc->getY())))
                return pin;
        }
    }

    return nullptr;
}

void GraphEditorPanel::resized()
{
    updateComponents();
}

void GraphEditorPanel::changeListenerCallback (ChangeBroadcaster*)
{
    updateComponents();
}

void GraphEditorPanel::updateComponents()
{
    for (int i = getNumChildComponents(); --i >= 0;)
    {
        if (FilterComponent* const fc = dynamic_cast <FilterComponent*> (getChildComponent (i)))
            fc->update();
    }

    for (int i = getNumChildComponents(); --i >= 0;)
    {
        ConnectorComponent* const cc = dynamic_cast <ConnectorComponent*> (getChildComponent (i));

        if (cc != nullptr && cc != draggingConnector)
        {
            if (graph.getConnectionBetween (cc->sourceFilterID, cc->sourceFilterChannel,
                                            cc->destFilterID, cc->destFilterChannel) == nullptr)
            {
                delete cc;
            }
            else
            {
                cc->update();
            }
        }
    }

    for (int i = graph.getNumFilters(); --i >= 0;)
    {
        const AudioProcessorGraph::Node::Ptr f (graph.getNode (i));

        if (getComponentForFilter (f->nodeId) == 0)
        {
            FilterComponent* const comp = new FilterComponent (graph, f->nodeId);
            addAndMakeVisible (comp);
            comp->update();
        }
    }

    for (int i = graph.getNumConnections(); --i >= 0;)
    {
        const AudioProcessorGraph::Connection* const c = graph.getConnection (i);

        if (getComponentForConnection (*c) == 0)
        {
            ConnectorComponent* const comp = new ConnectorComponent (graph);
            addAndMakeVisible (comp);

            comp->setInput (c->sourceNodeId, c->sourceChannelIndex);
            comp->setOutput (c->destNodeId, c->destChannelIndex);
        }
    }
}

void GraphEditorPanel::beginConnectorDrag (const uint32 sourceFilterID, const int sourceFilterChannel,
        const uint32 destFilterID, const int destFilterChannel,
        const MouseEvent& e)
{
    draggingConnector = dynamic_cast <ConnectorComponent*> (e.originalComponent);

    if (draggingConnector == nullptr)
        draggingConnector = new ConnectorComponent (graph);

    draggingConnector->setInput (sourceFilterID, sourceFilterChannel);
    draggingConnector->setOutput (destFilterID, destFilterChannel);

    addAndMakeVisible (draggingConnector);
    draggingConnector->toFront (false);

    dragConnector (e);
}

void GraphEditorPanel::dragConnector (const MouseEvent& e)
{
    const MouseEvent e2 (e.getEventRelativeTo (this));

    if (draggingConnector != nullptr)
    {
        draggingConnector->setTooltip (String::empty);

        int x = e2.x;
        int y = e2.y;

        if (PinComponent* const pin = findPinAt (x, y))
        {
            uint32 srcFilter = draggingConnector->sourceFilterID;
            int srcChannel   = draggingConnector->sourceFilterChannel;
            uint32 dstFilter = draggingConnector->destFilterID;
            int dstChannel   = draggingConnector->destFilterChannel;

            if (srcFilter == 0 && ! pin->isInput)
            {
                srcFilter = pin->filterID;
                srcChannel = pin->index;
            }
            else if (dstFilter == 0 && pin->isInput)
            {
                dstFilter = pin->filterID;
                dstChannel = pin->index;
            }

            if (graph.canConnect (srcFilter, srcChannel, dstFilter, dstChannel))
            {
                x = pin->getParentComponent()->getX() + pin->getX() + pin->getWidth() / 2;
                y = pin->getParentComponent()->getY() + pin->getY() + pin->getHeight() / 2;

                draggingConnector->setTooltip (pin->getTooltip());
            }
        }

        if (draggingConnector->sourceFilterID == 0)
            draggingConnector->dragStart (x, y);
        else
            draggingConnector->dragEnd (x, y);
    }
}

void GraphEditorPanel::endDraggingConnector (const MouseEvent& e)
{
    if (draggingConnector == nullptr)
        return;

    draggingConnector->setTooltip (String::empty);

    const MouseEvent e2 (e.getEventRelativeTo (this));

    uint32 srcFilter = draggingConnector->sourceFilterID;
    int srcChannel   = draggingConnector->sourceFilterChannel;
    uint32 dstFilter = draggingConnector->destFilterID;
    int dstChannel   = draggingConnector->destFilterChannel;

    draggingConnector = nullptr;

    if (PinComponent* const pin = findPinAt (e2.x, e2.y))
    {
        if (srcFilter == 0)
        {
            if (pin->isInput)
                return;

            srcFilter = pin->filterID;
            srcChannel = pin->index;
        }
        else
        {
            if (! pin->isInput)
                return;

            dstFilter = pin->filterID;
            dstChannel = pin->index;
        }

        graph.addConnection (srcFilter, srcChannel, dstFilter, dstChannel);
    }
}


//==============================================================================
class TooltipBar   : public Component,
    private Timer
{
public:
    TooltipBar()
    {
        startTimer (100);
    }

    void paint (Graphics& g)
    {
        g.setFont (Font (getHeight() * 0.7f, Font::bold));
        g.setColour (Colours::black);
        g.drawFittedText (tip, 10, 0, getWidth() - 12, getHeight(), Justification::centredLeft, 1);
    }

    void timerCallback()
    {
        Component* const underMouse = Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
        TooltipClient* const ttc = dynamic_cast <TooltipClient*> (underMouse);

        String newTip;

        if (ttc != nullptr && ! (underMouse->isMouseButtonDown() || underMouse->isCurrentlyBlockedByAnotherModalComponent()))
            newTip = ttc->getTooltip();

        if (newTip != tip)
        {
            tip = newTip;
            repaint();
        }
    }

private:
    String tip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TooltipBar)
};

//==============================================================================
GraphDocumentComponent::GraphDocumentComponent (AudioPluginFormatManager& formatManager,
        AudioDeviceManager* deviceManager_)
    : graph (formatManager), deviceManager (deviceManager_)
{
    addAndMakeVisible (graphPanel = new GraphEditorPanel (graph));

    deviceManager->addChangeListener (graphPanel);

    graphPlayer.setProcessor (&graph.getGraph());

    keyState.addListener (&graphPlayer.getMidiMessageCollector());

    addAndMakeVisible (keyboardComp = new MidiKeyboardComponent (keyState,
            MidiKeyboardComponent::horizontalKeyboard));

	keyboardComp->setColour(MidiKeyboardComponent::ColourIds::whiteNoteColourId, Colours::white.darker(.3f));
	keyboardComp->setColour(MidiKeyboardComponent::ColourIds::blackNoteColourId, Colours::green.darker(.9f));
	keyboardComp->setColour(MidiKeyboardComponent::ColourIds::upDownButtonArrowColourId, Colours::lime);
	keyboardComp->setColour(MidiKeyboardComponent::ColourIds::upDownButtonBackgroundColourId, Colour(30,30,30));
	
	
	

    addAndMakeVisible (statusBar = new TooltipBar());

    deviceManager->addAudioCallback (&graphPlayer);
    deviceManager->addMidiInputCallback (String::empty, &graphPlayer.getMidiMessageCollector());

    graphPanel->updateComponents();
}

GraphDocumentComponent::~GraphDocumentComponent()
{
    deviceManager->removeAudioCallback (&graphPlayer);
    deviceManager->removeMidiInputCallback (String::empty, &graphPlayer.getMidiMessageCollector());
    deviceManager->removeChangeListener (graphPanel);

    deleteAllChildren();

    graphPlayer.setProcessor (nullptr);
    keyState.removeListener (&graphPlayer.getMidiMessageCollector());

    graph.clear();
}

void GraphDocumentComponent::resized()
{
    const int keysHeight = 60;
    const int statusHeight = 20;

    graphPanel->setBounds (0, 0, getWidth(), getHeight() - keysHeight);
    statusBar->setBounds (0, getHeight() - keysHeight - statusHeight, getWidth(), statusHeight);
    keyboardComp->setBounds (200, getHeight() - keysHeight, getWidth()-200, keysHeight);
}

void GraphDocumentComponent::createNewPlugin (const PluginDescription* desc, int x, int y, bool isNative, String filename)
{
    graphPanel->createNewPlugin (desc, x, y, isNative, filename);
}
