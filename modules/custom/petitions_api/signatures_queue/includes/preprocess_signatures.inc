<?php

/**
 * @file
 * The "preprocess signatures" workflow.
 *
 * Retrieve a batch of records from signatures_pending_validation_queue, store
 * in signatures_pending_validation table, delete from queue.
 *
 * Retrieve a batch of records from validations_queue, store in mysql
 * validations table, delete from queue.
 */

/**
 * Preprocesses signatures.
 *
 * This function should not be invoked directly. Use
 * signatures_queue_invoke_workflow() instead.
 *
 * @param string $job_id
 *   A unique identifier for the job, as created by
 *   _signatures_queue_create_job_id().
 * @param string $server_name
 *   (optional) An arbitrary identifier for the server processing the workflow.
 * @param string $worker_name
 *   (optional) An arbitrary identifier for the invoking process (e.g., cron
 *   job).
 * @param array $options
 *   (optional) An array of options. Defaults to an empty array. It is currently
 *   unused by this workflow.
 *
 * @return bool
 *   Returns TRUE if the workflow executes successfully or FALSE if not.
 *
 * @see signatures_queue_invoke_workflow()
 */
function _signatures_queue_preprocess_signatures($job_id, $server_name, $worker_name, array $options = array()) {
  $watchdog_suffix = _signatures_queue_watchdog_suffix('preprocess_signatures', $job_id, $server_name, $worker_name);

  // Processing is the same for the two queues so we loop through them.
  $queues = array(
    'signatures_pending_validation_queue' => 'signatures_pending_validation',
    'validations_queue' => 'validations',
  );
  foreach ($queues as $queue => $table) {
    // Preprocess a batch of signatures in $queue queue.
    $result = _signatures_queue_preprocess_signatures_batch($queue, $table, $watchdog_suffix);

    if ($result['saved']) {
      watchdog('signatures_queue', '@number records were retrieved and deleted from @queue and stored in database tables. @suffix', array(
        '@number' => $result['saved'],
        '@queue' => $queue,
        '@suffix' => $watchdog_suffix,
      ), WATCHDOG_INFO);
    }
    else {
      if ($result['queued']) {
        watchdog('signatures_queue', '@queue contains @number record(s), but none were retrieved. @suffix', array(
          '@queue' => $queue,
          '@number' => number_format($result['queued']),
          '@suffix' => $watchdog_suffix,
        ), WATCHDOG_ERROR);
      }
      else {
        watchdog('signatures_queue', '@queue is empty. No signatures will be preprocessed. @suffix', array(
          '@queue' => $queue,
          '@suffix' => $watchdog_suffix,
        ), WATCHDOG_INFO);
      }
    }

    if ($result['failed']) {
      watchdog('signatures_queue', '@number record(s) were left in @queue after failure to move them into the database. (Will try again.) @suffix', array(
        '@number' => $result['failed'],
        '@queue' => $queue,
        '@suffix' => $watchdog_suffix,
      ), WATCHDOG_ERROR);
    }
  }

  return TRUE;
}

/**
 * Retrieve a batch of pending signatures and store them in mysql.
 *
 * Retrieve a batch of records from queue_name,
 *   store in mysql $table_name table, delete from queue.
 *   (If we fail to insert into mysql, re-insert into queue)
 *
 * @param string $queue_name
 *   The name of the queue to process.
 *   Expecting either signatures_pending_validation_queue or
 *   validations_queue.
 * @param string $table_name
 *   The name of the table for storing records.
 *   Expecting either signatures_pending_validation or validations.
 * @param string $watchdog_suffix
 *   A string of job details as created by _signatures_queue_watchdog_suffix().
 *
 * @return array
 *   Array info about records moved into mysql and items remaining in queue
 *   - retrieved, count of items retrieved from queue to be moved to mysql
 *   - saved, count of records successfully inserted into mysql table
 *   - skipped, count of duplicated records that were already fetched from queue
 *   - failed, count of items not successfully moved from a queue into mysql
 *   - queued, total number of items currently in the queue
 */
function _signatures_queue_preprocess_signatures_batch($queue_name, $table_name, $watchdog_suffix) {
  $return = array('saved', 'skipped', 'failed', 'queued', 'retrieved');
  $return = array_combine($return, array_fill(0, count($return), 0));

  $queue = SignaturesQueue::get($queue_name);
  $queue->createQueue();
  $return['queued'] = $queue->numberOfItems();

  $max = signatures_queue_get_queue_batch_size('preprocess_signatures');

  for ($i = 0; ($i < $max && $queue->numberOfItems() > 0); $i++) {

    // It's possible for Amazon SQS to return empty objects, skip them.
    if (!$item = $queue->claimItem()) {
      watchdog('signatures_queue', 'Skipping record because it is empty. @suffix', array(
        '@suffix' => $watchdog_suffix,
      ), WATCHDOG_ERROR);
      continue;
    }

    // Save the item in signatures_processing db.
    // The $item->data array is conviniently the same as the field mapping we
    // need for the db_insert() call.
    if (array_key_exists('signup', $item->data)) {
      // Cast signup to int for database type compatibility.
      $item->data['signup'] = (int) $item->data['signup'];
    }
    try {
      // Check for secret_validation_key in validations_processed table.
      if ($table_name == 'validations' && _signatures_queue_item_in_validations_processed($item)) {
        // Skip inserting, validation link has already been processed.
        $queue->deleteItem($item);
        $return['skipped'] ++;

        // Reset db for db logging.
        watchdog('signatures_queue', 'Skipping record because this validation has already been processed (secret_validation_key %key exists in validations_processed table). This record has been deleted from the validations_queue queue. @suffix', array(
          '%key' => $item->data['secret_validation_key'],
          '@suffix' => $watchdog_suffix,
        ), WATCHDOG_NOTICE);
      }
      else {
        // Preprocess data to be inserted.
        signatures_queue_set_db();

        // Load the database schema for the appropriate table.
        module_load_install('signatures_queue');
        if ($queue_name == 'validations_queue') {
          $schema = _signatures_queue_get_validations_schema();
        }
        // signatures_pending_validation_queue
        else {
          $schema = _signatures_queue_get_signatures_schema();
        }
        $fields = array_keys($schema['fields']);
        $record = array_intersect_key((array) $item->data, array_flip($fields));

        $id = db_insert($table_name)
          ->fields($record)
          ->execute();
        db_set_active();

        $queue->deleteItem($item);
        $return['saved']++;
      }
    }
    catch (Exception $e) {
      $return['failed']++;

      // Reset db for db logging.
      watchdog('signatures_queue', "Error while attempting db insert. @suffix.\n%error", array(
        '%error' => $e->getMessage(),
        '@suffix' => $watchdog_suffix,
      ), WATCHDOG_ERROR);
    }
  }

  // Set the active database back to default.
  db_set_active();

  $return['retrieved'] = $i;

  return $return;
}

/**
 * Check if the item's secret_validation_key is in validations_processed table.
 */
function _signatures_queue_item_in_validations_processed($item) {
  signatures_queue_set_db();
  $result = db_select('validations_processed', 'vp')
    ->fields('vp', array('vid'))
    ->condition('secret_validation_key', $item->data['secret_validation_key'])
    ->execute();
  db_set_active();
  return (bool) $result->rowCount();
}
