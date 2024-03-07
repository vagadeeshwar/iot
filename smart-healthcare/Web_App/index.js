const express = require("express");
const socketIo = require("socket.io");
//https://socket.io/docs/v4/client-socket-instance/

const { EventHubConsumerClient } = require("@azure/event-hubs");
//https://www.npmjs.com/package/@azure/event-hubs
//https://learn.microsoft.com/en-in/javascript/api/%40azure/event-hubs/eventhubproducerclient?view=azure-node-latest

const app = express();
const httpServer = require("http").createServer(app);
const io = socketIo(httpServer);

const connectionString =
  "Endpoint=sb://ihsuprodmares016dednamespace.servicebus.windows.net/;SharedAccessKeyName=iothubowner;SharedAccessKey=7d/psPhy1fpSKt+pZaB2a9ij13J2rJFKiAIoTNS0KWk=;EntityPath=iothub-ehub-i-o-t-25328432-57339a8147";
const consumerGroup = "$Default";
const deviceId = "1";

app.use(express.static(__dirname + "/public"));

const consumerClient = new EventHubConsumerClient(
  consumerGroup,
  connectionString
);

async function startReceivingTelemetryData() {
  const subscription = consumerClient.subscribe(
    {
      processEvents: async (events, context) => {
        for (const event of events) {
          if (
            event.systemProperties["iothub-connection-device-id"] === deviceId
          ) {
            io.emit("telemetry", event.body);
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

startReceivingTelemetryData();

const port = process.env.PORT || 3000; // This bloody code snippet which determines the port number dynamically on azure took me half a day to debug
httpServer.listen(port, () => {
  // You are going to hell if you change this
  console.log(`Server listening on port ${port}`);
});
