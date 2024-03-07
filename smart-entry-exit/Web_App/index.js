// "use strict";
// var Client = require("azure-iothub").Client;
// var Message = require("azure-iot-common").Message;

const {
  EventHubConsumerClient,
  EventHubProducerClient,
} = require("@azure/event-hubs"); //https://www.npmjs.com/package/@azure/event-hubs
//https://learn.microsoft.com/en-in/javascript/api/%40azure/event-hubs/eventhubproducerclient?view=azure-node-latest

const express = require("express");
const app = express();
const httpServer = require("http").createServer(app);
const io = require("socket.io")(httpServer);
//https://socket.io/docs/v4/client-socket-instance/

const connectionString =
  "Endpoint=sb://ihsuprodmares016dednamespace.servicebus.windows.net/;SharedAccessKeyName=iothubowner;SharedAccessKey=7d/psPhy1fpSKt+pZaB2a9ij13J2rJFKiAIoTNS0KWk=;EntityPath=iothub-ehub-i-o-t-25328432-57339a8147";
const consumerGroup = "$Default";
const deviceId = "3";

app.use(express.static(__dirname + "/public"));

const consumerClient = new EventHubConsumerClient(
  consumerGroup,
  connectionString
);

const producerClient = new EventHubProducerClient(connectionString);

async function startReceivingTelemetryData() {
  const subscription = consumerClient.subscribe(
    {
      processEvents: async (events, context) => {
        for (const event of events) {
          if (
            event.systemProperties["iothub-connection-device-id"] === deviceId
          ) {
            io.emit("devicetocloud-telemetry", event.body);
          }
        }
      },
      processError: async (err, context) => {
        console.error(err);
      },
    },
    { startPosition: { enqueuedOn: Date.now() } }
);

  console.log("Receiving telemetry data...");
}

async function startSendingTelemetryData(data) {
  const eventDataBatch = await producerClient.createBatch();
  //  let numberOfEventsToSend = 10;

  //  while (numberOfEventsToSend > 0) {
  let wasAdded = eventDataBatch.tryAdd({ body: data });
  if (!wasAdded) {
    return;
  }
  //  numberOfEventsToSend--;
  //  }
  await producerClient.sendBatch(eventDataBatch);
  // await producerClient.close();
}

io.on("connection", (socket) => {
  startReceivingTelemetryData();

  socket.on("cloudtodevice-telemetry", (data) => {
    startSendingTelemetryData(data);
    console.log("Sent cloudtodevice telemetry");
  });
});

// startReceivingTelemetryData();

const port = process.env.PORT || 3000; // This bloody code snippet which determines the port number dynamically on azure took me half a day to debug
httpServer.listen(port, () => {
  // You are going to hell if you change this
  console.log(`Server listening on port ${port}`);
});
