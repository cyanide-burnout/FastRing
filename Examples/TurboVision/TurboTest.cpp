#define Uses_TApplication
#define Uses_TWindow
#define Uses_TView
#define Uses_TRect
#define Uses_TDrawBuffer
#define Uses_TMenuBar
#define Uses_TSubMenu
#define Uses_TMenuItem
#define Uses_TStatusLine
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TDeskTop
#define Uses_TKeys
#define Uses_MsgBox
#include <tvision/tv.h>

#include <stdio.h>
#include <errno.h>

#include "TurboRing.h"

#define TICK_INTERVAL  100

enum
{
  cmMessage = 100
};

// A view the ring changes: the ticks of a repeating timeout

class TickView : public TView
{
  public:

    unsigned long ticks;

    TickView(const TRect& bounds) :
      TView(bounds), ticks(0)
    {
      growMode = gfGrowHiX | gfGrowHiY;
    }

    void draw() override
    {
      TDrawBuffer buffer;
      TColorAttr color;
      char text[64];
      int line;

      color = getColor(1);
      snprintf(text, sizeof(text), " Ticks: %lu", ticks);

      for (line = 0; line < size.y; line ++)
      {
        buffer.moveChar(0, ' ', color, size.x);
        if (line == 1) buffer.moveStr(0, text, color);
        writeLine(0, line, size.x, 1, buffer);
      }
    }
};

// Application

class TurboTest : public TurboRing
{
  public:

    TickView* view;

    TurboTest(struct FastRing* ring) :
      TProgInit(&TurboTest::initStatusLine, &TurboTest::initMenuBar, &TApplication::initDeskTop),
      TurboRing(ring)
    {
      TWindow* window;

      window = new TWindow(TRect(1, 1, 21, 6), "FastRing", 1);
      view   = new TickView(window->getExtent().grow(-1, -1));

      window->insert(view);
      deskTop->insert(window);
    }

    void handleEvent(TEvent& event) override
    {
      TurboRing::handleEvent(event);

      if ((event.what == evCommand) &&
          (event.message.command == cmMessage))
      {
        // The ring keeps ticking under a modal view, its loop runs on the fiber as well
        messageBox("The ticks go on meanwhile", mfInformation | mfOKButton);
        clearEvent(event);
      }
    }

    static TMenuBar* initMenuBar(TRect bounds)
    {
      bounds.b.y = bounds.a.y + 1;

      return new TMenuBar(bounds,
        *new TSubMenu("~F~ile", kbAltF) +
          *new TMenuItem("~M~essage", cmMessage, kbF2, hcNoContext, "F2") +
          newLine() +
          *new TMenuItem("E~x~it", cmQuit, kbAltX, hcNoContext, "Alt-X"));
    }

    static TStatusLine* initStatusLine(TRect bounds)
    {
      bounds.a.y = bounds.b.y - 1;

      return new TStatusLine(bounds,
        *new TStatusDef(0, 0xffff) +
          *new TStatusItem("~F2~ Message", kbF2, cmMessage) +
          *new TStatusItem("~Alt-X~ Exit", kbAltX, cmQuit));
    }
};

static void HandleTick(struct FastRingDescriptor* descriptor)
{
  TurboTest* application;

  // The view is changed outside the fiber, touch() lets the application draw it in this iteration
  application = static_cast<TurboTest*>(descriptor->closure);
  application->view->ticks ++;
  application->view->drawView();
  application->touch();
}

int main()
{
  struct FastRing* ring;
  struct FastRingDescriptor* tick;
  TurboTest* application;
  int result;

  if ((ring = CreateFastRing(0)) == nullptr)
  {
    fprintf(stderr, "io_uring cannot be set up\n");
    return 1;
  }

  application = new TurboTest(ring);
  tick        = SetFastRingTimeout(ring, nullptr, TICK_INTERVAL, TIMEOUT_FLAG_REPEAT | TIMEOUT_FLAG_OWNED, HandleTick, application);

  result = application->start() ? 0 : -ENOMEM;

  while ((application->isRunning()) &&
         ((result >= 0) || (result == -EINTR)))
  {
    // The ring is the main loop, the application goes on whenever there is something for it
    result = WaitForFastRing(ring, 1000, nullptr);
  }

  SetFastRingTimeout(ring, tick, -1, 0, nullptr, nullptr);
  TObject::destroy(application);
  ReleaseFastRing(ring);

  return 0;
}
