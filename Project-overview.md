# Project Overview
The project is a room lighting manager for hospitals. It is meant to simulate a circadian day/night cycle.
Each room's light will have a receiver and each room has a sender (control panel).

## Control Panel (Sender)
The ESP8266 on the control panel will have a potentiometer knob for the user to spin to manually override and adjust their lighting.
The control panel will also have a button to toggle whether override is on or off. 
The control panel's potentiometer's lighting setting should only be used if the override for its respective room is on.
The control panel sends instructions so that the receiver (light controller) knows what level to set the light to.
The control panel should pull the set "Quiet Hours" time from Redis and start dimming the lights 90 minutes prior to the set quiet hours.
The control panel should also start brightening the lights 30 minutes before the set "Wake Up" time.
Whenever the override button (or website) toggles the override flag it is written to `room:{id}:override`, and the sender immediately honors that state.

## Light Controller (Receiver)
The receiver receives instructions on what level to set the light to from Redis.

## Website
The website is how the user in their respective room will be able to change their preferred "Quiet Hours"
Each room should have its own path (ex. www.website.com/room1)
The website should allow the user to:
- Change their preferred quiet hours (when to go to sleep, which would mean complete darkness, and when to wake up, which would mean lights fully on)
- Be able to toggle the manual control panel brightness override (same thing as the button on the control panel)

Behind the scenes the website stores quiet-hour selections in `room:{id}:cfg` and the override state in `room:{id}:override`, so both the bedside hardware and the browser stay synchronized via Redis.
